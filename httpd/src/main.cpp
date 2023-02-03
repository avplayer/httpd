﻿//
// main.cpp
// ~~~~~~~~
//
// Copyright (c) 2022 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include "httpd/logging.hpp"
#include "httpd/scoped_exit.hpp"
#include "httpd/use_awaitable.hpp"
#include "httpd/misc.hpp"
#include "httpd/strutil.hpp"
#include "httpd/publish_subscribe.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace beast = boost::beast;	// from <boost/beast/http.hpp>
namespace http = beast::http;

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/stream_file.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
namespace net = boost::asio;
using net::ip::tcp;

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <boost/signals2.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/url.hpp>
#include <boost/regex.hpp>

#include <fmt/xchar.h>
#include <fmt/format.h>

#include <map>
#include <deque>
#include <string>
#include <string_view>
#include <chrono>


//////////////////////////////////////////////////////////////////////////

using string_body = http::string_body;
using string_response = http::response<string_body>;

using buffer_body = http::buffer_body;
using buffer_response = http::response<buffer_body>;
using response_serializer = http::response_serializer<buffer_body, http::fields>;

using dynamic_body = http::dynamic_body;
using dynamic_request = http::request<dynamic_body>;
using request_parser = http::request_parser<dynamic_request::body_type>;
using flat_buffer = beast::flat_buffer;

// All io objects use net::io_context::executor_type instead
// any_io_executor for better performance

using executor_type = net::io_context::executor_type;

using tcp_stream = beast::basic_stream<
	tcp, executor_type, beast::unlimited_rate_policy>;

using tcp_resolver = net::ip::basic_resolver<tcp, executor_type>;
using tcp_acceptor = net::basic_socket_acceptor<tcp, executor_type>;
using tcp_socket = net::basic_stream_socket<tcp, executor_type>;

using steady_timer = net::basic_waitable_timer<
	std::chrono::steady_clock,
	net::wait_traits<std::chrono::steady_clock>, executor_type>;

using awaitable_void = net::awaitable<void, executor_type>;

//////////////////////////////////////////////////////////////////////////


const auto global_buffer_size = 5 * 1024 * 1024;

constexpr static auto head_fmt =
LR"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
constexpr static auto tail_fmt =
L"</pre><hr></body></html>";
constexpr static auto body_fmt =
L"<a href=\"{}\">{}</a>{}{}              {}\r\n";

const static std::string satisfiable_html =
R"x1x(<html>
<head><title>416 Requested Range Not Satisfiable</title></head>
<body>
<center><h1>416 Requested Range Not Satisfiable</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>
)x1x";

const static std::map<std::string, std::string> global_mimes =
{
	{ ".html", "text/html; charset=utf-8" },
	{ ".htm", "text/html; charset=utf-8" },
	{ ".js", "application/javascript" },
	{ ".h", "text/javascript" },
	{ ".hpp", "text/javascript" },
	{ ".cpp", "text/javascript" },
	{ ".cxx", "text/javascript" },
	{ ".cc", "text/javascript" },
	{ ".c", "text/javascript" },
	{ ".json", "application/json" },
	{ ".css", "text/css" },
	{ ".woff", "application/x-font-woff" },
	{ ".pdf", "application/pdf" },
	{ ".png", "image/png" },
	{ ".jpg", "image/jpg" },
	{ ".jpeg", "image/jpg" },
	{ ".gif", "image/gif" },
	{ ".webp", "image/webp" },
	{ ".svg", "image/svg+xml" },
	{ ".wav", "audio/x-wav" },
	{ ".ogg", "video/ogg" },
	{ ".mp4", "video/mp4" },
	{ ".flv", "video/x-flv" },
	{ ".f4v", "video/x-f4v" },
	{ ".ts", "video/MP2T" },
	{ ".mov", "video/quicktime" },
	{ ".avi", "video/x-msvideo" },
	{ ".wmv", "video/x-ms-wmv" },
	{ ".3gp", "video/3gpp" },
	{ ".mkv", "video/x-matroska" },
	{ ".7z", "application/x-7z-compressed" },
	{ ".ppt", "application/vnd.ms-powerpoint" },
	{ ".zip", "application/zip" },
	{ ".xz", "application/x-xz" },
	{ ".xml", "application/xml" },
	{ ".webm", "video/webm" }
};

bool global_pipe = false;
std::string global_path;
publish_subscribe global_publish_subscribe;
bool global_quit = false;

using ranges = std::vector<std::pair<int64_t, int64_t>>;

static inline ranges get_ranges(std::string range)
{
	range = strutil::remove_spaces(range);
	boost::ireplace_first(range, "bytes=", "");

	boost::sregex_iterator it(
		range.begin(), range.end(),
		boost::regex{ "((\\d+)-(\\d+))+" });

	ranges result;
	std::for_each(it, {}, [&result](const auto& what) mutable
		{
			result.emplace_back(
				std::make_pair(
					std::atoll(what[2].str().c_str()),
					std::atoll(what[3].str().c_str())));
		});

	if (result.empty() && !range.empty())
	{
		if (range.front() == '-')
		{
			auto r = std::atoll(range.c_str());
			result.emplace_back(std::make_pair(r, -1));
		}
		else if (range.back() == '-')
		{
			auto r = std::atoll(range.c_str());
			result.emplace_back(std::make_pair(r, -1));
		}
	}

	return result;
}

static inline std::filesystem::path path_cat(
	const std::wstring& doc, const std::wstring& target)
{
	size_t start_pos = 0;
	for (auto& c : target)
	{
		if (!(c == L'/' || c == '\\'))
			break;

		start_pos++;
	}

	std::wstring_view sv;
	std::wstring slash = L"/";

	if (start_pos < target.size())
		sv = std::wstring_view(target.c_str() + start_pos);

#ifdef WIN32
	slash = L"\\";
	if (doc.back() == L'/' ||
		doc.back() == L'\\')
		slash = L"";

	return std::filesystem::path(doc + slash + std::wstring(sv));
#else
	if (doc.back() == L'/')
		slash = L"";

	return std::filesystem::path(
		boost::nowide::narrow(doc + slash + std::wstring(sv)));
#endif // WIN32
};

awaitable_void read_from_stdin()
{
	auto ex = co_await net::this_coro::executor;

#ifdef __linux__
#ifdef BOOST_ASIO_HAS_IO_URING
	net::basic_stream_file<executor_type> is(ex);
#else
	net::posix::basic_stream_descriptor<executor_type> is(ex);
#endif
#elif defined(_WIN32)
	net::basic_stream_file<executor_type> is(ex);
#endif

	boost::system::error_code ec;

#ifdef _WIN32
	auto stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
	is.assign(stdin_handle, ec);
#else
	is.assign(::dup(STDIN_FILENO), ec);
#endif

	if (ec)
	{
		LOG_ERR << "Open stdin error: " << ec.message();
		co_return;
	}

	LOG_DBG << "Open stdin successfully";

	scoped_exit se([]()
		{
			global_quit = true;
			LOG_DBG << "Quit read from stdin";
		});

	steady_timer timer(ex);

	while (true)
	{
		auto size = global_publish_subscribe.size();
		if (size == 0)
		{
			if (!is.is_open())
				break;

			timer.expires_from_now(std::chrono::milliseconds(100));
			co_await timer.async_wait(ioc_awaitable[ec]);
			continue;
		}

		for (;;)
		{
			publish_subscribe::data_type data =
				std::make_shared<std::vector<uint8_t>>(
					data_length);

			auto gcount =
				co_await is.async_read_some(
					net::buffer(data->data(), data_length),
					ioc_awaitable[ec]);
			if (gcount <= 0)
				break;

			data->resize(gcount);
			global_publish_subscribe.publish(data);

			if (global_publish_subscribe.size() == 0)
			{
				LOG_DBG << "No client connection";
				break;
			}

			if (ec)
				break;
		}
	}

	co_return;
}

static inline awaitable_void error_session(
	tcp_stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	http::status code,
	const std::string& message)
{
	string_response res{
		code,
		req.version()
	};

	res.set(http::field::server, "httpd/1.0");
	res.set(http::field::content_type, "text/html");
	res.keep_alive(req.keep_alive());
	res.body() = message;
	res.prepare_payload();

	boost::beast::http::serializer<
		false, string_body, http::fields> sr{ res };

	boost::system::error_code ec;
	co_await http::async_write(stream,
		sr,
		ioc_awaitable[ec]);
	if (ec)
	{
		LOG_ERR << "Session: "
			<< connection_id
			<< ", async_write: "
			<< ec.message();
	}

	co_return;
}

static inline awaitable_void pipe_session(
	tcp_stream& stream, dynamic_request& req, int64_t connection_id)
{
	boost::system::error_code ec;

	using buffer_queue_type = std::deque<publish_subscribe::data_type>;
	buffer_queue_type buffer_queue;

	auto ex = co_await net::this_coro::executor;
	steady_timer notify(ex);

	auto fetch_data =
		[&buffer_queue, &notify]
	(publish_subscribe::data_type data) mutable
	{
		buffer_queue.push_back(data);
		notify.cancel_one();
	};

	auto subscribe_handle = global_publish_subscribe.subscribe(fetch_data);
	scoped_exit se_unsub([&subscribe_handle]() mutable
		{
			global_publish_subscribe.unsubscribe(subscribe_handle);
		});

	auto& lowest_layer = beast::get_lowest_layer(stream);
	lowest_layer.expires_after(std::chrono::seconds(60));

	buffer_response res{
		http::status::ok,
		req.version()
	};

	res.set(http::field::server, "httpd/1.0");
	res.set(http::field::content_type, "text/html");
	res.keep_alive(req.keep_alive());
	int64_t file_size = -1;

	response_serializer sr(res);

	res.body().data = nullptr;
	res.body().more = false;

	co_await http::async_write_header(
		stream,
		sr,
		ioc_awaitable[ec]);
	if (ec)
	{
		LOG_ERR << "Session: "
			<< connection_id
			<< ", async_write_header: "
			<< ec.message();
		co_return;
	}

	do
	{
		if (buffer_queue.empty())
		{
			if (file_size == 0)
				break;

			notify.expires_from_now(std::chrono::seconds(60));
			co_await notify.async_wait(ioc_awaitable[ec]);

			continue;
		}

		auto p = buffer_queue.front();
		buffer_queue.pop_front();
		if (!p)
		{
			res.body().data = nullptr;
			res.body().more = false;
		}
		else
		{
			res.body().data = p->data();
			res.body().size = p->size();
			res.body().more = true;
		}

		co_await http::async_write(
			stream,
			sr,
			ioc_awaitable[ec]);
		if (ec == http::error::need_buffer)
		{
			file_size -= p->size();
			ec = {};
			continue;
		}

		if (ec)
		{
			LOG_ERR << "Session: "
				<< connection_id
				<< ", async_write body: "
				<< ec.message();
			co_return;
		}
	} while (!sr.is_done());

	co_return;
}

static inline awaitable_void dir_session(
	tcp_stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	std::filesystem::path dir)
{
	LOG_DBG << "Session: "
		<< connection_id
		<< ", path: "
		<< dir;

	boost::system::error_code ec;

	std::filesystem::directory_iterator end;
	std::filesystem::directory_iterator it(dir, ec);

	if (ec || dir.string().back() != '/')
	{
		string_response res{
			http::status::found,
			req.version()
		};

		auto dirs = boost::nowide::narrow(dir.u16string());
		dirs = boost::replace_first_copy(dirs, global_path, "") + "/";

		res.set(http::field::location, dirs);
		res.keep_alive(req.keep_alive());
		res.prepare_payload();

		http::serializer<
			false,
			string_body,
			http::fields> sr(res);

		co_await http::async_write(
			stream,
			sr,
			ioc_awaitable[ec]);

		if (ec)
			LOG_ERR << "Session: "
			<< connection_id
			<< ", err: "
			<< ec.message();

		co_return;
	}

	std::vector<std::wstring> path_list;

	for (; it != end; it++)
	{
		const auto& item = it->path();
		std::filesystem::path realpath = item;
		std::wstring time_string;

		auto ftime = std::filesystem::last_write_time(item, ec);
		if (ec)
		{
#ifdef WIN32
			if (item.string().size() > MAX_PATH)
			{
				auto str = item.string();
				boost::replace_all(str, "/", "\\");
				realpath = "\\\\?\\" + str;
				ftime = std::filesystem::last_write_time(realpath, ec);
			}
#endif
		}

		const auto stime =
			std::chrono::time_point_cast<
			std::chrono::system_clock::duration>(
				ftime -
				std::filesystem::file_time_type::clock::now() +
				std::chrono::system_clock::now());

		const auto write_time =
			std::chrono::system_clock::to_time_t(stime);

		char tmbuf[64] = { 0 };
		auto tm = std::localtime(&write_time);

		if (tm)
		{
			std::strftime(
				tmbuf,
				sizeof(tmbuf),
				"%m-%d-%Y %H:%M",
				tm);
		}

		time_string = boost::nowide::widen(tmbuf);
		std::wstring wstr;

		if (std::filesystem::is_directory(realpath, ec))
		{
			auto leaf = item.filename().u16string();
			leaf = leaf + u"/";
			wstr.assign(leaf.begin(), leaf.end());

			int width = 50 - ((int)leaf.size() + 1);
			width = width < 0 ? 10 : width;
			std::wstring space(width, L' ');

			auto str = fmt::format(body_fmt,
				wstr,
				wstr,
				space,
				time_string,
				L"[DIRECTORY]");

			path_list.push_back(str);
		}
		else
		{
			auto leaf = item.filename().u16string();
			wstr.assign(leaf.begin(), leaf.end());

			auto sz = static_cast<float>(
				std::filesystem::file_size(
					realpath, ec));
			if (ec)
				sz = 0;

			std::wstring filesize =
				boost::nowide::widen(
					strutil::add_suffix(sz));

			int width = 50 - (int)leaf.size();
			width = width < 0 ? 10 : width;
			std::wstring space(width, L' ');

			auto str = fmt::format(body_fmt,
				wstr,
				wstr,
				space,
				time_string,
				filesize);

			path_list.push_back(str);
		}
	}

	auto dirs = boost::nowide::narrow(dir.u16string());
	dirs = boost::replace_first_copy(dirs, global_path, "");
	auto root_path =
		boost::nowide::widen(dirs);

	std::wstring head =
		fmt::format(
			head_fmt,
			root_path,
			root_path);

	std::wstring body =
		fmt::format(
			body_fmt,
			L"../",
			L"../",
			L"",
			L"",
			L"");

	std::sort(path_list.begin(), path_list.end());
	for (auto& s : path_list)
		body += s;
	body = head + body + tail_fmt;

	string_response res{
		http::status::ok,
		req.version()
	};

	res.keep_alive(req.keep_alive());
	res.body() = boost::nowide::narrow(body);
	res.prepare_payload();

	http::serializer<
		false,
		string_body,
		http::fields> sr(res);

	co_await http::async_write(
		stream,
		sr,
		ioc_awaitable[ec]);

	if (ec)
		LOG_ERR << "Session: "
		<< connection_id
		<< ", err: "
		<< ec.message();

	co_return;
}

static inline awaitable_void file_session(
	tcp_stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	std::filesystem::path file)
{
	LOG_DBG << "Session: "
		<< connection_id
		<< ", file: "
		<< file;

	boost::system::error_code ec;
	size_t content_length = std::filesystem::file_size(file, ec);

	if (req.method() != http::verb::get)
	{
		co_await error_session(
			stream,
			req,
			connection_id,
			http::status::bad_request,
			"Bad request");

		co_return;
	}

#ifdef WIN32
	auto filename = file.wstring();
	boost::replace_all(filename, "/", "\\");
	// Windows use unc path workaround.
	filename = L"\\\\?\\" + filename;
	file = filename;
#endif
	if (!std::filesystem::exists(file))
	{
		co_await error_session(
			stream,
			req,
			connection_id,
			http::status::not_found,
			"Not Found");

		co_return;
	}

	std::fstream file_stream(
		file.string(),
		std::ios_base::binary |
		std::ios_base::in);

	auto range = get_ranges(req["Range"]);
	http::status st = http::status::ok;

	if (!range.empty())
	{
		st = http::status::partial_content;
		auto& r = range.front();

		if (r.second == -1)
		{
			if (r.first < 0)
			{
				r.first = content_length + r.first;
				r.second = content_length - 1;
			}
			else if (r.first >= 0)
			{
				r.second = content_length - 1;
			}
		}

		file_stream.seekg(r.first, std::ios_base::beg);
	}

	buffer_response res{ st, req.version() };
	res.set(http::field::server, "httpd/1.0");
	auto ext = strutil::to_lower(file.extension().string());

	if (global_mimes.count(ext))
		res.set(http::field::content_type, global_mimes.at(ext));
	else
		res.set(http::field::content_type, "text/plain");

	if (st == http::status::ok)
		res.set(http::field::accept_ranges, "bytes");

	if (st == http::status::partial_content)
	{
		const auto& r = range.front();

		if (r.second < r.first && r.second >= 0)
		{
			co_await error_session(
				stream,
				req,
				connection_id,
				http::status::range_not_satisfiable,
				satisfiable_html);

			co_return;
		}

		std::string content_range = fmt::format(
			"bytes {}-{}/{}",
			r.first,
			r.second,
			content_length);

		content_length = r.second - r.first + 1;
		res.set(http::field::content_range, content_range);
	}

	res.keep_alive(req.keep_alive());
	res.content_length(content_length);

	response_serializer sr(res);

	res.body().data = nullptr;
	res.body().more = false;

	co_await http::async_write_header(
		stream,
		sr,
		ioc_awaitable[ec]);
	if (ec)
	{
		LOG_WARN << "Session: "
			<< connection_id
			<< ", async_write_header: "
			<< ec.message();

		co_return;
	}

	char bufs[global_buffer_size];
	std::streamsize total = 0;

	do
	{
		file_stream.read(bufs, global_buffer_size);

		auto bytes_transferred = std::min<std::streamsize>(
			file_stream.gcount(),
			content_length - total);

		if (bytes_transferred == 0 ||
			total >= (std::streamsize)content_length)
		{
			res.body().data = nullptr;
			res.body().more = false;
		}
		else
		{
			res.body().data = bufs;
			res.body().size = bytes_transferred;
			res.body().more = true;
		}

		co_await http::async_write(
			stream,
			sr,
			ioc_awaitable[ec]);

		total += bytes_transferred;
		if (ec == http::error::need_buffer)
		{
			ec = {};
			continue;
		}
		if (ec)
		{
			LOG_WARN << "Session: "
				<< connection_id
				<< ", async_write: "
				<< ec.message();
			co_return;
		}
	} while (!sr.is_done());

	co_return;
}

static inline awaitable_void session(tcp_stream stream)
{
	static int64_t static_connection_id = 0;
	int64_t connection_id = static_connection_id++;

	boost::system::error_code ec;

	std::string remote_host;
	auto endp = stream.socket().remote_endpoint(ec);
	if (!ec)
	{
		if (endp.address().is_v6())
		{
			remote_host = "[" + endp.address().to_string()
				+ "]:" + std::to_string(endp.port());
		}
		else
		{
			remote_host = endp.address().to_string()
				+ ":" + std::to_string(endp.port());
		}
	}

	LOG_DBG << "Session: "
		<< connection_id
		<< ", host: "
		<< remote_host
		<< " is coming...";

	scoped_exit se_quit([&]()
		{
			LOG_DBG << "Session: " << connection_id << ", left...";
		});

	flat_buffer buffer;
	buffer.reserve(global_buffer_size);

	bool keep_alive = false;

	for (;;)
	{
		request_parser parser;
		parser.body_limit(std::numeric_limits<uint64_t>::max());

		co_await http::async_read_header(stream,
			buffer,
			parser,
			ioc_awaitable[ec]);

		if (ec)
			co_return;

		if (parser.get()[http::field::expect] == "100-continue")
		{
			http::response<http::empty_body> res;
			res.version(11);
			res.result(http::status::continue_);

			co_await http::async_write(stream,
				res,
				ioc_awaitable[ec]);
			if (ec)
			{
				LOG_ERR << "Session: "
					<< connection_id
					<< ", expect async_write: "
					<< ec.message();
				co_return;
			}
		}

		dynamic_request req = parser.release();

		if (beast::websocket::is_upgrade(req))
			co_return;

		if (global_pipe)
		{
			co_await pipe_session(
				stream,
				req,
				connection_id);

			co_return;
		}

		if (std::filesystem::is_regular_file(global_path))
		{
			co_await file_session(
				stream,
				req,
				connection_id,
				global_path);

			if (keep_alive)
				continue;
			co_return;
		}

		if (!std::filesystem::is_directory(global_path))
		{
			co_await error_session(
				stream,
				req,
				connection_id,
				http::status::internal_server_error,
				"internal server error");

			if (keep_alive)
				continue;
			co_return;
		}

		keep_alive = req.keep_alive();
		std::string target;

		strutil::unescape(
			{
				req.target().data(),
				req.target().size()
			},
			target);

		auto current_path = path_cat(
			boost::nowide::widen(global_path),
			boost::nowide::widen(target));

		if (std::filesystem::is_directory(current_path))
		{
			co_await dir_session(
				stream,
				req,
				connection_id,
				current_path);

			if (keep_alive)
				continue;
			co_return;
		}

		if (std::filesystem::is_regular_file(current_path))
		{
			co_await file_session(
				stream,
				req,
				connection_id,
				current_path);

			if (keep_alive)
				continue;
			co_return;
		}

		if (!std::filesystem::exists(current_path))
		{
			co_await error_session(
				stream,
				req,
				connection_id,
				http::status::not_found,
				"Not Found");

			if (keep_alive)
				continue;
			co_return;
		}

		co_await error_session(
			stream,
			req,
			connection_id,
			http::status::bad_request,
			"Bad request");

		if (keep_alive)
			continue;
		co_return;
	}

	co_return;
}

static inline awaitable_void listen(tcp_acceptor& acceptor)
{
	for (;;)
	{
		boost::system::error_code ec;

		auto client =
			co_await acceptor.async_accept(
				ioc_awaitable[ec]);
		if (ec)
			break;

		{
			net::socket_base::keep_alive option(true);
			client.set_option(option, ec);
		}

		{
			net::ip::tcp::no_delay option(true);
			client.set_option(option, ec);
		}

		auto ex(client.get_executor());
		tcp_stream stream(std::move(client));

		co_spawn(
			ex,
			session(std::move(stream)),
			net::detached);
	}

	co_return;
}


int main(int argc, char** argv)
{
	platform_init();

	std::string httpd_listen;

	// 解析命令行.
	po::options_description desc("Options");
	desc.add_options()
		("help,h", "Help message.")
		("listen", po::value<std::string>(&httpd_listen)->default_value("[::0]:80")->value_name("ip:port"), "Httpd tcp listen.")
		("file", po::value<std::string>(&global_path)->value_name("file/pipe"), "Filename or pipe.")
		;

	po::variables_map vm;
	po::store(
		po::command_line_parser(argc, argv)
		.options(desc)
		.style(po::command_line_style::unix_style
			| po::command_line_style::allow_long_disguise)
		.run()
		, vm);
	po::notify(vm);

	// 帮助输出.
	if (vm.count("help") || argc == 1)
	{
		std::cout << desc;
		return EXIT_SUCCESS;
	}

	std::string host;
	std::string port;
	bool v6only;

	// 解析侦听端口.
	if (!parse_endpoint_string(
		httpd_listen,
		host,
		port,
		v6only))
	{
		std::cerr << "Cannot parse listen: " << httpd_listen << "\n";
		return EXIT_FAILURE;
	}

	net::io_context ctx;

	auto listen_endpoint =
		*tcp_resolver(ctx).resolve(
			host,
			port,
			tcp_resolver::passive
		);

	tcp_acceptor acceptor(ctx, listen_endpoint);

	// 启动tcp侦听.
	for (int i = 0; i < 16; i++)
	{
		net::co_spawn(
			ctx.get_executor(),
			listen(acceptor),
			net::detached);
	}

	// 如果是pipe, 则直接启动文件读.
	if (global_path.empty() || global_path == "-")
	{
		global_pipe = true;

		net::co_spawn(
			ctx.get_executor(),
			read_from_stdin(),
			net::detached);
	}

	ctx.run();

	return EXIT_SUCCESS;
}
