//
// httpd.cpp
// ~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// g++ httpd.cpp -static-libstdc++ -static-libgcc -pthread -Wl,-Bstatic -lboost_system -lboost_thread -lboost_filesystem -lboost_coroutine -lboost_context -o httpd
//

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <deque>
#include <atomic>
#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/asio.hpp>
#include <boost/thread/condition.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/future.hpp>
#include <boost/thread/once.hpp>
#include <boost/filesystem.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/signals2.hpp>

std::atomic_int once;

using namespace boost::filesystem;
using boost::asio::ip::tcp;

const int max_length = 1024 * 1024 * 2;
std::string filename;

typedef boost::shared_ptr<tcp::socket> socket_ptr;
struct buffer
{
	boost::array<char, max_length> buf;
	std::size_t size;
};

typedef boost::shared_ptr<buffer> buffer_ptr;
boost::mutex buffer_signal_mtu;
boost::signals2::signal<void(buffer_ptr)> buffer_signal;
typedef std::deque<buffer_ptr> buffer_queue;
bool abort_read_pipe = false;
bool keep_on_end = false;

template <class Func>
void read_file(std::string filename, Func& func)
{
	boost::async(boost::launch::async, [filename, func]()
	{
		std::istream& is = std::cin;
		for (; !is.eof();)
		{
			auto buf = boost::make_shared<buffer>();
			is.read(buf->buf.data(), max_length);
			buf->size = is.gcount();
			boost::mutex::scoped_lock lock(buffer_signal_mtu);
			buffer_signal(buf);
		}

		if (filename.empty())
		{
			abort_read_pipe = true;
			boost::mutex::scoped_lock lock(buffer_signal_mtu);
			buffer_signal.disconnect_all_slots();
		}
		else
		{
			boost::mutex::scoped_lock lock(buffer_signal_mtu);
			buffer_signal.disconnect(func);
		}
	});
}

class http_session : public boost::enable_shared_from_this<http_session>
{
public:
	explicit http_session(boost::asio::io_service& io_service)
		: strand_(io_service)
		, socket_(io_service)
		, timer_(io_service)
		, file_size_(0)
	{}

	~http_session()
	{
		std::cout << "http " << this << " has been destoryed!" << std::endl;
	}

	void go()
	{
		boost::asio::spawn(strand_,
			boost::bind(&http_session::do_work,
				shared_from_this(), _1));
	}

	tcp::socket& socket()
	{
		return socket_;
	}

protected:

	void wait_for_close()
	{
		timer_.expires_from_now(std::chrono::seconds(5));
		auto self = shared_from_this();
		boost::asio::spawn(strand_, [this, self](boost::asio::yield_context yield)
		{
			while (socket_.is_open())
			{
				boost::system::error_code ignored_ec;
				timer_.async_wait(yield[ignored_ec]);
				if (timer_.expires_from_now() <= std::chrono::seconds(0) && socket_.is_open())
				{
					std::cout << "http " << this << " write data timeout!\n";
					socket_.close(ignored_ec);
					if (!keep_on_end && filename.empty() && abort_read_pipe)
					{
						strand_.get_io_service().stop();
					}
				}
			}
		});
	}

	void do_work(boost::asio::yield_context yield)
	{
		try
		{
			std::cout << "http " << this << " connected!" << std::endl;

			boost::asio::streambuf buf;
			boost::system::error_code ec;
			auto n = boost::asio::async_read_until(socket_, buf, "\r\n\r\n", yield[ec]);
			if (ec)
			{
				std::cout << "http " << this << " error: " << ec.message() << std::endl;
				return;
			}

			bool abort = false;
			if (filename.empty() && abort_read_pipe)
			{
				abort = true;
			}

			boost::asio::streambuf response;
			std::ostream response_stream(&response);
			if (abort)
				response_stream << "HTTP/1.1 404 OK\r\n";
			else
				response_stream << "HTTP/1.1 200 OK\r\n";
			if (!filename.empty())
			{
				file_size_ = file_size(filename, ec);
				if (!ec && file_size_ > 0)
				{
					response_stream << "Content-Length: " << file_size_ << "\r\n";
				}
			}
			response_stream << "Connection: close\r\n";
			response_stream << "\r\n";
			n = boost::asio::async_write(socket_, response, yield[ec]);
			if (ec)
			{
				std::cout << "http " << this << " error: " << ec.message() << std::endl;
				return;
			}

			if (abort)
				return;

			if (!filename.empty())
			{
				std::ifstream file;
				file.open(filename.c_str(), std::ios::binary);
				if (file.bad())
				{
					std::cout << "Unable to open file " << filename << std::endl;
					return;
				}
				std::istream& is = file;
				char* buf = new char[max_length];
				std::unique_ptr<char> bufptr(buf);
				for (; !is.eof();)
				{
					is.read(buf, max_length);
					auto size = is.gcount();
					boost::asio::async_write(socket_,
						boost::asio::buffer(buf, size), yield[ec]);
					if (ec)
					{
						std::cout << "http " << this
							<< " write data error: " << ec.message() << std::endl;
						return;
					}
				}

				wait_for_close();

				return;
			}
			else
			{
				auto q = boost::make_shared<buffer_queue>();
				auto write_func = boost::bind(&http_session::write_buffer, shared_from_this(), q, _1);
				auto read_file_func = read_file<decltype(write_func)>;

				{
					boost::mutex::scoped_lock lock(buffer_signal_mtu);
					buffer_signal.connect(write_func);
				}

				if (once++ == 0)
					read_file_func(filename, write_func);

				wait_for_close();

				while (true)
				{
					auto n = boost::asio::async_read(socket_, buf, yield[ec]);
					if (ec)
					{
						std::cout << "http " << this << " error: " << ec.message() << std::endl;
						{
							boost::mutex::scoped_lock lock(buffer_signal_mtu);
							buffer_signal.disconnect(write_func);
						}
						std::cout << "http " << this << " disconnect, num of slots "
							<< buffer_signal.num_slots() << std::endl;
						socket_.close(ec);
						return;
					}
					buf.consume(n);
				}
			}
		}
		catch (std::exception& e)
		{
			std::cerr << "do work " << e.what() << std::endl;
			boost::system::error_code ignore_ec;
			socket_.close(ignore_ec);
		}
	}

	void write_buffer(boost::shared_ptr<buffer_queue> q, buffer_ptr buf)
	{
		auto& queue = *q;
		bool write_in_progress = !queue.empty();
		queue.push_back(buf);

		timer_.expires_from_now(std::chrono::seconds(5));

		if (!write_in_progress)
		{
			auto self = shared_from_this();
			strand_.post([self, this, q]() {
				boost::asio::spawn(strand_,
					boost::bind(&http_session::do_write,
						self, q, _1));
			});
		}
	}

	void do_write(boost::shared_ptr<buffer_queue> q, boost::asio::yield_context yield)
	{
		if (!q)
			return;
		auto& bq = *q;
		while (!bq.empty())
		{
			timer_.expires_from_now(std::chrono::seconds(5));
			auto& buf = *bq.front();
			boost::system::error_code ec;
			auto n = boost::asio::async_write(socket_,
				boost::asio::buffer(buf.buf, buf.size), yield[ec]);
			if (ec)
			{
				socket_.close(ec);
				break;
			}
			bq.pop_front();
		}
	}

private:
	boost::asio::io_service::strand strand_;
	tcp::socket socket_;
	boost::asio::steady_timer timer_;
	std::size_t file_size_;
};

void do_accept(boost::asio::io_service& io_service,
	tcp::acceptor& acceptor, boost::asio::yield_context yield)
{
	for (;;)
	{
		boost::system::error_code ec;
		boost::shared_ptr<http_session> new_session(new http_session(io_service));
		acceptor.async_accept(new_session->socket(), yield[ec]);
		if (ec) break;
		new_session->go();
	}
}

int main(int argc, char* argv[])
{
	try
	{
		if (argc < 2)
		{
			std::cerr << "Usage: " << argv[0] << " <port> [keep] [file|pipe]\n";
			return 1;
		}

		once = 0;
		boost::asio::io_service io_service;

		if (argc > 2)
			filename = argv[2];
		if (filename == "keep")
		{
			keep_on_end = true;
			filename = "";
		}

		using namespace std; // For atoi.

		tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), atoi(argv[1])));

		for (int i = 0; i < 16; i++)
		{
			boost::asio::spawn(io_service,
				boost::bind(do_accept,
					boost::ref(io_service), boost::ref(acceptor), _1));
		}

		io_service.run();
	}
	catch (std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}

