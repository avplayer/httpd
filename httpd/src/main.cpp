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

#include "httpd/logging.hpp"
#include "httpd/scoped_exit.hpp"
#include "httpd/use_awaitable.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
namespace http = boost::beast::http;           // from <boost/beast/http.hpp>

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

#include <array>
#include <map>
#include <deque>
#include <iostream>
#include <string>
#include <span>
#include <string_view>

#ifdef __linux__
#  include <sys/resource.h>

# ifndef HAVE_UNAME
#  define HAVE_UNAME
# endif

#elif _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>

#endif // _WIN32

#ifdef HAVE_UNAME
#  include <sys/utsname.h>
#endif

//////////////////////////////////////////////////////////////////////////

using string_body = http::string_body;
using string_response = http::response<string_body>;

using buffer_body = http::buffer_body;
using buffer_response = http::response<buffer_body>;
using response_serializer = http::response_serializer<buffer_body, http::fields>;

using dynamic_body = http::dynamic_body;
using dynamic_request = http::request<dynamic_body>;
using request_parser = http::request_parser<dynamic_request::body_type>;


//////////////////////////////////////////////////////////////////////////


inline bool is_space(const char c)
{
	if (c == ' ' ||
		c == '\f' ||
		c == '\n' ||
		c == '\r' ||
		c == '\t' ||
		c == '\v')
		return true;
	return false;
}


//////////////////////////////////////////////////////////////////////////

inline std::string_view string_trim(std::string_view sv)
{
	const char* b = sv.data();
	const char* e = b + sv.size();

	for (; b != e; b++)
	{
		if (!is_space(*b))
			break;
	}

	for (; e != b; )
	{
		if (!is_space(*(--e)))
		{
			++e;
			break;
		}
	}

	return std::string_view(b, e - b);
}


//////////////////////////////////////////////////////////////////////////

inline bool parse_endpoint_string(std::string_view str,
	std::string& host, std::string& port, bool& ipv6only)
{
	ipv6only = false;

	auto address_string = string_trim(str);
	auto it = address_string.begin();

	bool is_ipv6_address = *it == '[';
	if (is_ipv6_address)
	{
		auto host_end = std::find(it, address_string.end(), ']');
		if (host_end == address_string.end())
			return false;

		it++;
		for (auto first = it; first != host_end; first++)
			host.push_back(*first);

		std::advance(it, host_end - it);
		it++;
	}
	else
	{
		auto host_end = std::find(it, address_string.end(), ':');
		if (host_end == address_string.end())
			return false;

		for (auto first = it; first != host_end; first++)
			host.push_back(*first);

		// Skip host.
		std::advance(it, host_end - it);
	}

	if (*it != ':')
		return false;

	it++;
	for (; it != address_string.end(); it++)
	{
		if (*it >= '0' && *it <= '9')
		{
			port.push_back(*it);
			continue;
		}

		break;
	}

	if (it != address_string.end())
	{
#ifdef __cpp_lib_to_address
		auto opt = std::string_view(
			std::to_address(it), address_string.end() - it);
#else
		auto opt = std::string(it, address_string.end());
#endif
		if (opt == "ipv6only" || opt == "-ipv6only")
			ipv6only = true;
	}

	return true;
}


//////////////////////////////////////////////////////////////////////////

inline int platform_init()
{
#if defined(WIN32) || defined(_WIN32)
	/* Disable the "application crashed" popup. */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
		SEM_NOOPENFILEERRORBOX);

#if defined(DEBUG) ||defined(_DEBUG)
	//	_CrtDumpMemoryLeaks();
	// 	int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
	// 	flags |= _CRTDBG_LEAK_CHECK_DF;
	// 	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	// 	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
	// 	_CrtSetDbgFlag(flags);
#endif

#if !defined(__MINGW32__)
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif

	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	_setmode(2, _O_BINARY);

	/* Disable stdio output buffering. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* Enable minidump when application crashed. */
#elif defined(__linux__)
	rlimit of = { 50000, 100000 };
	setrlimit(RLIMIT_NOFILE, &of);

	struct rlimit core_limit;
	core_limit.rlim_cur = RLIM_INFINITY;
	core_limit.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &core_limit);

	/* Set the stack size programmatically with setrlimit */
	rlimit rl;
	int result = getrlimit(RLIMIT_STACK, &rl);
	if (result == 0)
	{
		const rlim_t stack_size = 100 * 1024 * 1024;
		if (rl.rlim_cur < stack_size)
		{
			rl.rlim_cur = stack_size;
			setrlimit(RLIMIT_STACK, &rl);
		}
	}
#endif

	std::ios::sync_with_stdio(false);

	return 0;
}

//////////////////////////////////////////////////////////////////////////

// static std::map<std::string, std::string> mime_map =
// {
// 	{ ".html", "text/html; charset=utf-8" },
// 	{ ".js", "application/javascript" },
// 	{ ".css", "text/css" },
// 	{ ".woff", "application/x-font-woff" },
// 	{ ".png", "image/png" },
// 	{ ".jpg", "image/jpg" },
// 	{ ".wav", "audio/x-wav" },
// 	{ ".mp4", "video/mp4" }
// };

const inline int data_length = 2 * 1024 * 1024;

class publish_subscribe
{
public:
	using data_type = std::shared_ptr<std::vector<uint8_t>>;
	using subscribe_func_type = std::function<void(data_type)>;
	using subscribe_list_type = boost::signals2::signal<void(data_type)>;

public:
	publish_subscribe() = default;
	~publish_subscribe() = default;

public:
	boost::signals2::connection sub(subscribe_func_type f) noexcept
	{
		return subscribes_.connect(f);
	}

	void unsub(const boost::signals2::connection& slot) noexcept
	{
		slot.disconnect();
	}

	void perform(data_type data) noexcept
	{
		subscribes_(data);
	}

	size_t size() const
	{
		return subscribes_.num_slots();
	}

	void clear() noexcept
	{
		subscribes_.disconnect_all_slots();
	}

private:
	subscribe_list_type subscribes_;
};

std::string global_filename;
publish_subscribe global_publish_subscribe;

net::awaitable<void> readfile(std::string filename)
{
	auto ex = co_await net::this_coro::executor;

	net::steady_timer timer(ex);

#ifdef __linux__
#ifdef BOOST_ASIO_HAS_IO_URING
	net::stream_file is(ex);
#else
	net::posix::stream_descriptor is(ex);
#endif
#elif defined(_WIN32)
	net::stream_file is(ex);
#endif

	boost::system::error_code ec;
	bool pipe = false;

	if (filename.empty() || filename == "-")
	{
#ifdef _WIN32
		auto stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
		is.assign(stdin_handle, ec);
#else
		is.assign(::dup(STDIN_FILENO), ec);
#endif
		pipe = true;
	}
	else
	{
#ifdef __linux__
#ifdef BOOST_ASIO_HAS_IO_URING
		is.open(filename, net::stream_file::read_only, ec);
#else
		auto fd = ::open(filename.c_str(), O_RDONLY | O_DIRECT);
		is.assign(fd, ec);
#endif
#elif defined(_WIN32)
		is.open(filename, net::stream_file::read_only, ec);
#endif
	}

	if (ec)
	{
		LOG_ERR << "readfile: "
			<< filename
			<< " error: "
			<< ec.message();
		co_return;
	}

	LOG_DBG << "Open readfile: " << filename;
	scoped_exit se([&filename]()
		{
			LOG_DBG << "Quit readfile: " << filename;
		});

restart:

	while (true)
	{
		auto size = global_publish_subscribe.size();
		if (size == 0)
		{
			timer.expires_from_now(std::chrono::milliseconds(100));
			co_await timer.async_wait(asio_util::use_awaitable[ec]);
			continue;
		}

		for (;;)
		{
			publish_subscribe::data_type data =
				std::make_shared<std::vector<uint8_t>>(
					data_length);

			auto gcount = co_await is.async_read_some(
				net::buffer(data->data(),
					data_length),
				asio_util::use_awaitable[ec]);
			if (gcount <= 0)
				break;

			data->resize(gcount);
			global_publish_subscribe.perform(data);

			if (global_publish_subscribe.size() == 0)
				goto restart;
			if (ec)
				break;
		}

		if (!pipe)
			co_return;
	}

	co_return;
}

net::awaitable<void> session(boost::beast::tcp_stream stream)
{
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

	LOG_DBG << "Client: " << remote_host << " is coming...";

	auto ex = co_await net::this_coro::executor;

	using buffer_queue_type = std::deque<publish_subscribe::data_type>;
	buffer_queue_type buffer_queue;

	net::steady_timer timer(ex);

	auto fetch_data =
		[&buffer_queue, &timer]
		(publish_subscribe::data_type data) mutable
		{
			buffer_queue.push_back(data);
			timer.cancel_one();
		};

	auto subscribe_handle = global_publish_subscribe.sub(fetch_data);
	scoped_exit se_unsub([&subscribe_handle]() mutable
		{
			global_publish_subscribe.unsub(subscribe_handle);
		});

	scoped_exit se_quit([&remote_host]()
		{
			LOG_DBG << "Session: " << remote_host << " left...";
		});

	boost::beast::flat_buffer buffer;

	const auto httpd_receive_buffer_size = 5 * 1024 * 1024;
	buffer.reserve(httpd_receive_buffer_size);

	for (;;)
	{
		request_parser parser;
		parser.body_limit(std::numeric_limits<uint64_t>::max());

		co_await http::async_read_header(stream,
			buffer,
			parser,
			asio_util::use_awaitable[ec]);
		if (ec)
		{
			LOG_ERR << remote_host
				<< ", async_read_header: "
				<< ec.message();
			co_return;
		}

		if (parser.get()[http::field::expect] == "100-continue")
		{
			http::response<http::empty_body> res;
			res.version(11);
			res.result(http::status::continue_);

			co_await http::async_write(stream,
				res,
				asio_util::use_awaitable[ec]);
			if (ec)
			{
				LOG_ERR << remote_host
					<< ", expect async_write: "
					<< ec.message();
				co_return;
			}
		}

		auto req = parser.release();
		std::string target = req.target().to_string();
		if (!boost::beast::websocket::is_upgrade(req))
		{
			bool pipe = true;
			bool is_dir = std::filesystem::is_directory(global_filename);
			std::string target_filename = global_filename;

			if (is_dir)
			{
				target_filename = global_filename + target;
				if (!std::filesystem::exists(target_filename) ||
					std::filesystem::is_directory(target_filename))
				{
					boost::system::error_code ec;
					string_response res {
						http::status::bad_request,
						req.version()
					};
					res.set(http::field::server, "httpd/1.0");
					res.set(http::field::content_type, "text/html");
					res.keep_alive(req.keep_alive());
					res.body() = "file not exists";
					res.prepare_payload();

					boost::beast::http::serializer<false,
						string_body,
						http::fields> sr{ res };
					co_await http::async_write(stream,
						sr,
						asio_util::use_awaitable[ec]);
					if (ec)
					{
						LOG_ERR << remote_host
							<< ", async_write: "
							<< ec.message();
					}

					co_return;
				}

				pipe = false;

				net::co_spawn(ex,
					readfile(target_filename),
					net::detached);
			}
			else
			{
				// 如果是pipe, 则直接启动文件读.
				if (!target_filename.empty() && target_filename != "-")
				{
					pipe = false;

					net::co_spawn(ex,
						readfile(target_filename),
						net::detached);
				}
			}

			auto& lowest_layer = boost::beast::get_lowest_layer(stream);
			lowest_layer.expires_after(std::chrono::seconds(60));

			buffer_response res{
				http::status::ok,
				req.version()
			};
			res.set(http::field::server, "httpd/1.0");
			res.set(http::field::content_type, "text/html");
			res.keep_alive(req.keep_alive());
			int64_t file_size = -1;
			if (!pipe)
			{
				file_size = std::filesystem::file_size(target_filename);
				res.content_length(file_size);
			}

			response_serializer sr{ res };

			res.body().data = nullptr;
			res.body().more = false;

			co_await http::async_write_header(
				stream,
				sr,
				asio_util::use_awaitable[ec]);
			if (ec)
			{
				LOG_ERR << remote_host
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

					timer.expires_from_now(std::chrono::seconds(60));
					co_await timer.async_wait(asio_util::use_awaitable[ec]);
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
					asio_util::use_awaitable[ec]);
				if (ec == http::error::need_buffer)
				{
					file_size -= p->size();
					ec = {};
					continue;
				}

				if (ec)
				{
					LOG_ERR << remote_host
						<< ", async_write body: "
						<< ec.message();
					co_return;
				}
			} while (!sr.is_done());

			co_return;
		}

		LOG_ERR << remote_host << ", upgrade to websocket not supported!";
		co_return;
	}

	co_return;
}

net::awaitable<void> listen(tcp::acceptor& acceptor)
{
	for (;;)
	{
		boost::system::error_code ec;

		auto client = co_await acceptor.async_accept(
			asio_util::use_awaitable[ec]);
		if (ec)
			break;

		{
			boost::asio::socket_base::keep_alive option(true);
			client.set_option(option, ec);
		}

		{
			boost::asio::ip::tcp::no_delay option(true);
			client.set_option(option, ec);
		}

		auto ex = client.get_executor();
		co_spawn(ex,
			session(boost::beast::tcp_stream(std::move(client))),
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
		("file", po::value<std::string>(&global_filename)->default_value("")->value_name("file/pipe"), "Filename or pipe.")
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

	net::io_context ctx;
	std::string host;
	std::string port;
	bool v6only;

	// 解析侦听端口.
	if (!parse_endpoint_string(httpd_listen, host, port, v6only))
	{
		std::cerr << "Cannot parse listen: " << httpd_listen << "\n";
		return EXIT_FAILURE;
	}

	auto listen_endpoint =
		*tcp::resolver(ctx).resolve(
			host,
			port,
			tcp::resolver::passive
		);

	tcp::acceptor acceptor(ctx, listen_endpoint);

	// 启动tcp侦听.
	for (int i = 0; i < 16; i++)
	{
		net::co_spawn(ctx,
			listen(acceptor),
			net::detached);
	}

	// 如果是pipe, 则直接启动文件读.
	if (global_filename.empty() || global_filename == "-")
	{
		net::co_spawn(ctx,
			readfile(global_filename),
			net::detached);
	}

	ctx.run();

	return EXIT_SUCCESS;
}
