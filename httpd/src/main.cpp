//
// main.cpp
// ~~~~~~~~
//
// Copyright (c) 2022 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//

#include <map>
#include <deque>
#include <string>
#include <string_view>
#include <chrono>
#include <tuple>

#include <fmt/xchar.h>
#include <fmt/format.h>

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

#ifdef USE_STD_FILESYSTEM
# include <filesystem>
namespace fs = std::filesystem;
#else
# include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#endif

#include <boost/signals2.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/regex.hpp>

#include "httpd/logging.hpp"
#include "httpd/scoped_exit.hpp"
#include "httpd/use_awaitable.hpp"
#include "httpd/misc.hpp"
#include "httpd/strutil.hpp"
#include "httpd/publish_subscribe.hpp"


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

inline fs::path addLongPathAware(const fs::path& p)
{
	auto w = p.wstring();
#ifdef _WIN32
	boost::replace_all(w, L"/", L"\\");
	if (w.size() < 4 || !w.starts_with(L"\\\\?\\"))
		w = L"\\\\?\\" + w;
#endif
	return fs::path{ w };
}

inline fs::path removeLongPathAware(const fs::path& p)
{
	auto w = p.wstring();
#ifdef _WIN32
	boost::replace_all(w, L"/", L"\\");
	if (w.size() > 4 && w.starts_with(L"\\\\?\\"))
		w.erase(0, 4);
#endif
	return fs::path{ w };
}

using strutil::add_suffix;


//////////////////////////////////////////////////////////////////////////


const auto global_buffer_size = 5 * 1024 * 1024;

constexpr static auto head_fmt =
LR"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
constexpr static auto tail_fmt =
L"</pre><hr></body></html>";
constexpr static auto body_fmt =
L"<a href=\"{}\">{}</a>{} {}       {}\r\n";

const static std::string satisfiable_html =
R"x1x(<html>
<head><title>416 Requested Range Not Satisfiable</title></head>
<body>
<center><h1>416 Requested Range Not Satisfiable</h1></center>
<hr><center>nginx/1.20.2</center>
</body>
</html>
)x1x";

inline const char* version_string = "nginx/1.20.2";

const static std::map<std::string, std::string> global_mimes =
{
	{ ".html", "text/html; charset=utf-8" },
	{ ".htm", "text/html; charset=utf-8" },
	{ ".js", "application/javascript; charset=utf-8" },
	{ ".h", "text/javascript; charset=utf-8" },
	{ ".hpp", "text/javascript; charset=utf-8" },
	{ ".cpp", "text/javascript; charset=utf-8" },
	{ ".cxx", "text/javascript; charset=utf-8" },
	{ ".cc", "text/javascript; charset=utf-8" },
	{ ".c", "text/javascript; charset=utf-8" },
	{ ".json", "application/json; charset=utf-8" },
	{ ".css", "text/css; charset=utf-8" },
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
	{ ".m4a", "audio/mp4" },
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
	{ ".xml", "application/xml; charset=utf-8" },
	{ ".webm", "video/webm" }
};

bool global_pipe = false;
fs::path global_path;
publish_subscribe global_publish_subscribe;
bool global_quit = false;

using ranges = std::vector<std::pair<int64_t, int64_t>>;

inline ranges get_ranges(std::string range)
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

inline std::string server_date_string()
{
	auto time = std::time(nullptr);
	auto gmt = gmtime((const time_t*)&time);

	std::string str(64, '\0');
	auto ret = strftime((char*)str.data(), 64, "%a, %d %b %Y %H:%M:%S GMT", gmt);
	str.resize(ret);

	return str;
}

inline awaitable_void read_from_stdin()
{
	auto ex = co_await net::this_coro::executor;

#ifdef __linux__
#  ifdef BOOST_ASIO_HAS_IO_URING
	net::basic_stream_file<executor_type> is(ex);
#  else
	net::posix::basic_stream_descriptor<executor_type> is(ex);
#  endif
#elif defined(__APPLE__)
	net::posix::basic_stream_descriptor<executor_type> is(ex);
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

inline awaitable_void error_session(
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

inline awaitable_void pipe_session(
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

inline std::tuple<std::string, fs::path> file_last_wirte_time(const fs::path& file)
{
	static auto loc_time = [](auto t) -> struct tm*
	{
		using time_type = std::decay_t<decltype(t)>;
		if constexpr (std::is_same_v<time_type, std::filesystem::file_time_type>)
		{
			auto sctp = std::chrono::time_point_cast<
				std::chrono::system_clock::duration>(t -
					std::filesystem::file_time_type::clock::now() +
						std::chrono::system_clock::now());
			auto time = std::chrono::system_clock::to_time_t(sctp);
			return std::localtime(&time);
		}
		else if constexpr (std::is_same_v<time_type, std::time_t>)
		{
			return std::localtime(&t);
		}
		else
		{
			static_assert(!std::is_same_v<time_type, time_type>, "time type required!");
		}
	};

	boost::system::error_code ec;
	std::string time_string;
	fs::path unc_path;

	auto ftime = fs::last_write_time(file, ec);
	if (ec)
	{
#ifdef WIN32
		if (file.string().size() > MAX_PATH)
		{
			auto str = file.string();
			boost::replace_all(str, "/", "\\");
			unc_path = "\\\\?\\" + str;
			ftime = fs::last_write_time(unc_path, ec);
		}
#endif
	}

	if (!ec)
	{
		auto tm = loc_time(ftime);

		char tmbuf[64] = { 0 };
		std::strftime(tmbuf,
			sizeof(tmbuf),
			"%m-%d-%Y %H:%M",
			tm);

		time_string = tmbuf;
	}

	return { time_string, unc_path };
}

inline std::vector<std::wstring> format_path_list(const std::set<fs::path>& paths)
{
	boost::system::error_code ec;
	std::vector<std::wstring> path_list;

	for (auto it = paths.cbegin(); it != paths.cend(); it++)
	{
		const auto& item = *it;

		auto [ftime, unc_path] = file_last_wirte_time(item);
		std::wstring time_string = boost::nowide::widen(ftime);

		std::wstring rpath;

		if (fs::is_directory(item, ec))
		{
			auto leaf = boost::nowide::narrow(item.filename().wstring());
			leaf = leaf + "/";
			rpath = boost::nowide::widen(leaf);
			int width = 50 - rpath.size();
			width = width < 0 ? 0 : width;
			std::wstring space(width, L' ');
			auto show_path = rpath;
			if (show_path.size() > 50) {
				show_path = show_path.substr(0, 47);
				show_path += L"..&gt;";
			}
			auto str = fmt::format(body_fmt,
				rpath,
				show_path,
				space,
				time_string,
				L"-");

			path_list.push_back(str);
		}
		else
		{
			auto leaf = boost::nowide::narrow(item.filename().wstring());
			rpath = boost::nowide::widen(leaf);
			int width = 50 - (int)rpath.size();
			width = width < 0 ? 0 : width;
			std::wstring space(width, L' ');
			std::wstring filesize;
			if (unc_path.empty())
				unc_path = item;
			auto sz = static_cast<float>(fs::file_size(
				unc_path, ec));
			if (ec)
				sz = 0;
			filesize = boost::nowide::widen(
				add_suffix(sz));
			auto show_path = rpath;
			if (show_path.size() > 50) {
				show_path = show_path.substr(0, 47);
				show_path += L"..&gt;";
			}
			auto str = fmt::format(body_fmt,
				rpath,
				show_path,
				space,
				time_string,
				filesize);

			path_list.push_back(str);
		}
	}

	return path_list;
}

inline awaitable_void dir_session(
	tcp_stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	fs::path dir)
{
	LOG_DBG << "Session: "
		<< connection_id
		<< ", path: "
		<< dir;

	boost::system::error_code ec;

	fs::directory_iterator end;
	fs::directory_iterator it(dir, ec);

	if (ec)
	{
		LOG_WARN << "Session: "
			<< connection_id
			<< ", path: "
			<< dir
			<< ", err: "
			<< ec.message();

		co_await error_session(
			stream,
			req,
			connection_id,
			http::status::internal_server_error,
			"Internal server error");

		co_return;
	}

	// 遍历目录, 生成目录列表和文件列表.
	std::set<fs::path> dirs;
	std::set<fs::path> files;

	for (; it != end; it++)
	{
		const auto& item = it->path();

		if (fs::is_directory(item, ec))
			dirs.insert(item);
		else
			files.insert(item);
	}

	std::vector<std::wstring> path_list;

	path_list = format_path_list(dirs);
	auto file_list = format_path_list(files);
	path_list.insert(path_list.end(), file_list.begin(), file_list.end());

	auto current_dir = dir.wstring();
	auto root_path = boost::replace_first_copy(current_dir, global_path.wstring(), L"");

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

	for (auto& s : path_list)
		body += s;
	body = head + body + tail_fmt;

	string_response res{
		http::status::ok,
		req.version()
	};

	res.set(http::field::server, version_string);
	res.set(http::field::date, server_date_string());
	res.set(http::field::content_type, "text/html; charset=UTF-8");
	res.keep_alive(req.keep_alive());
	res.content_length(body.size());
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

inline awaitable_void file_session(
	tcp_stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	fs::path file)
{
	LOG_DBG << "Session: "
		<< connection_id
		<< ", file: "
		<< file;

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
	if (!fs::exists(file))
	{
		co_await error_session(
			stream,
			req,
			connection_id,
			http::status::not_found,
			"Not Found");

		co_return;
	}

	boost::system::error_code ec;
	size_t content_length = fs::file_size(file, ec);

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
		res.set(http::field::content_type, "text/plain; charset=utf-8");

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

	std::vector<char> buffer(global_buffer_size);
	char* bufs = buffer.data();
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

inline awaitable_void session(tcp_stream stream)
{
	static int64_t static_connection_id = 0;
	static size_t num_connections = 0;

	int64_t connection_id = static_connection_id++;
	num_connections++;

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
			num_connections--;

			LOG_DBG << "Session: "
				<< connection_id
				<< ", left, num connection: "
				<< num_connections
				<< "...";
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

		if (fs::is_regular_file(global_path))
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

		if (!fs::is_directory(global_path))
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
		if (!target.empty() && target[0] == '/')
			target.erase(0, 1);

		// 构造完整路径以及根据请求的目标构造路径.
		auto current_path = fs::canonical(global_path /
			boost::nowide::widen(target), ec).make_preferred();

		if (!current_path.wstring().starts_with(global_path.wstring()) || ec)
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

		auto realpath = addLongPathAware(current_path);

		if (fs::is_directory(realpath))
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

		if (fs::is_regular_file(realpath))
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

		if (!fs::exists(realpath))
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

inline awaitable_void listen(tcp_acceptor& acceptor)
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

		net::co_spawn(
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
	std::string httpd_doc;

	// 解析命令行.
	po::options_description desc("Options");
	desc.add_options()
		("help,h", "Help message.")
		("listen", po::value<std::string>(&httpd_listen)->default_value("[::0]:80")->value_name("ip:port"), "Httpd tcp listen.")
		("path", po::value<std::string>(&httpd_doc)->value_name("file/dir/pipe"), "Filename or directory or pipe.")
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
	if (httpd_doc.empty() || httpd_doc == "-")
	{
		global_pipe = true;

		net::co_spawn(
			ctx.get_executor(),
			read_from_stdin(),
			net::detached);
	}
	else
	{
		global_path = fs::canonical(fs::path(httpd_doc).make_preferred());
	}

	ctx.run();

	return EXIT_SUCCESS;
}
