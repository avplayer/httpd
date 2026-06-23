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
#include <type_traits>

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

#include <boost/asio/ssl.hpp>
namespace ssl = boost::asio::ssl;

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "httpd/logging.hpp"
#include "httpd/scoped_exit.hpp"
#include "httpd/use_awaitable.hpp"
#include "httpd/misc.hpp"
#include "httpd/strutil.hpp"
#include "httpd/publish_subscribe.hpp"
#include "httpd/lfs.hpp"


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
using awaitable_string = net::awaitable<std::string, executor_type>;

// SSL stream type wrapping tcp_stream.
using ssl_stream = ssl::stream<tcp_stream>;

// Global SSL context.
std::shared_ptr<ssl::context> global_ssl_ctx;

// Certificate info discovered from cert directory.
struct ssl_cert_info
{
    fs::path cert_file;
    fs::path key_file;
    std::string domain;
};

// Extract the first domain name (CN or SAN) from a PEM certificate file.
inline std::string extract_domain_from_cert(const fs::path& cert_file)
{
    BIO* bio = BIO_new_file(cert_file.string().c_str(), "r");
    if (!bio)
        return {};

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert)
        return {};

    std::string domain;

    // Try SAN first.
    GENERAL_NAMES* names = (GENERAL_NAMES*)X509_get_ext_d2i(
        cert, NID_subject_alt_name, nullptr, nullptr);
    if (names)
    {
        int num = sk_GENERAL_NAME_num(names);
        for (int i = 0; i < num; i++)
        {
            const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
            if (name->type == GEN_DNS)
            {
                const char* dns = (const char*)ASN1_STRING_get0_data(
                    name->d.dNSName);
                if (dns && *dns)
                {
                    domain = dns;
                    break;
                }
            }
        }
        GENERAL_NAMES_free(names);
    }

    // Fallback to CN.
    if (domain.empty())
    {
#if OPENSSL_VERSION_MAJOR >= 3
        const X509_NAME* subj = X509_get_subject_name(cert);
#else
        X509_NAME* subj = X509_get_subject_name(cert);
#endif
        if (subj)
        {
            char cn[256] = {};
            int len = X509_NAME_get_text_by_NID(
                subj, NID_commonName, cn, sizeof(cn));
            if (len > 0)
                domain = cn;
        }
    }

    X509_free(cert);

    // Skip wildcard prefix for display.
    if (domain.size() > 2 && domain[0] == '*' && domain[1] == '.')
        domain = domain.substr(2);

    return domain;
}

// Count how many certificates are in a PEM file (chain depth).
inline int count_cert_chain_depth(const fs::path& file)
{
    BIO* bio = BIO_new_file(file.string().c_str(), "r");
    if (!bio)
        return 0;

    int count = 0;
    X509* cert = nullptr;
    while ((cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) != nullptr)
    {
        count++;
        X509_free(cert);
    }
    BIO_free(bio);
    return count;
}

// Check if a PEM file contains a private key.
inline int check_key_file(const fs::path& file)
{
    BIO* bio = BIO_new_file(file.string().c_str(), "r");
    if (!bio)
        return 0;

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey)
        return 0;

    EVP_PKEY_free(pkey);
    return 100;
}

// Score a file as a certificate candidate by extension and chain depth.
// Returns 0 if not a certificate, or the chain depth (>0) as the score.
inline int score_cert_file(const fs::path& p)
{
    auto ext = p.extension().string();
    if (ext != ".pem" && ext != ".crt" && ext != ".cert")
        return 0;
    return count_cert_chain_depth(p);
}

// Score a file as a private key candidate by extension and content.
// Returns 0 if not a key, or 100 if it is a valid private key.
inline int score_key_file(const fs::path& p)
{
    auto ext = p.extension().string();
    if (ext != ".pem" && ext != ".key")
        return 0;
    return check_key_file(p);
}

// Extract the common prefix from a stem (before first '-' or '_').
// Well-known names (fullchain, cert, privkey, key) are mapped to "_default_".
inline std::string extract_cert_prefix(const std::string& stem, bool is_cert)
{
    static const std::set<std::string> default_certs = {"fullchain", "cert"};
    static const std::set<std::string> default_keys  = {"privkey", "key"};

    if ((is_cert && default_certs.count(stem)) ||
        (!is_cert && default_keys.count(stem)))
        return "_default_";

    auto sep = stem.find_first_of("-_");
    if (sep != std::string::npos)
        return stem.substr(0, sep);
    return stem;
}

// Collect files from cert_dir and partition into cert/key candidate maps by prefix.
inline void collect_certificate_files(
    const fs::path& cert_dir,
    std::map<std::string, std::vector<fs::path>>& cert_candidates,
    std::map<std::string, std::vector<fs::path>>& key_candidates,
    std::vector<fs::path>& all_files)
{
    boost::system::error_code ec;
    fs::directory_iterator end;
    fs::directory_iterator it(cert_dir, ec);
    if (ec)
    {
        XLOG_ERR << "Cannot open cert directory: "
            << cert_dir.string() << ", err: " << ec.message();
        return;
    }

    for (; it != end; it++)
    {
        if (!fs::is_regular_file(it->status()))
            continue;

        auto p = it->path();
        all_files.push_back(p);

        int cert_score = score_cert_file(p);
        int key_score  = score_key_file(p);

        if (cert_score > 0)
        {
            auto stem = p.stem().string();
            cert_candidates[extract_cert_prefix(stem, true)].push_back(p);
        }
        if (key_score > 0)
        {
            auto stem = p.stem().string();
            key_candidates[extract_cert_prefix(stem, false)].push_back(p);
        }
    }
}

// Match certificate files with key files by common prefix.
// Falls back to the default key pool when a specific prefix has no key.
inline void match_certs_with_keys(
    std::map<std::string, std::vector<fs::path>>& cert_candidates,
    std::map<std::string, std::vector<fs::path>>& key_candidates,
    std::vector<ssl_cert_info>& results)
{
    for (auto& [prefix, certs] : cert_candidates)
    {
        if (certs.empty())
            continue;

        // Pick the cert with the deepest chain.
        std::sort(certs.begin(), certs.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return score_cert_file(a) > score_cert_file(b);
            });

        auto& keys = key_candidates[prefix];
        if (keys.empty() && prefix != "_default_")
            keys = key_candidates["_default_"];

        if (keys.empty())
            continue;

        // Pick the best key (highest score — all valid keys score 100).
        std::sort(keys.begin(), keys.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return score_key_file(a) > score_key_file(b);
            });

        ssl_cert_info info;
        info.cert_file = certs.front();
        info.key_file  = keys.front();
        info.domain    = extract_domain_from_cert(info.cert_file);
        results.push_back(std::move(info));
    }
}

// Fallback: try every .pem/.crt as a cert and guess the key by common key names.
inline void fallback_scan_cert_pairs(
    const std::vector<fs::path>& all_files,
    std::vector<ssl_cert_info>& results)
{
    for (const auto& p : all_files)
    {
        auto ext = p.extension().string();
        if (ext != ".pem" && ext != ".crt" && ext != ".cert")
            continue;

        auto domain = extract_domain_from_cert(p);
        if (domain.empty())
            continue;

        auto dir  = p.parent_path();
        auto stem = p.stem().string();

        // Try common key naming conventions.
        auto try_existing = [&](const fs::path& kp)
        {
            if (fs::exists(kp))
            {
                ssl_cert_info info;
                info.cert_file = p;
                info.key_file  = kp;
                info.domain    = std::move(domain);
                results.push_back(std::move(info));
                return true;
            }
            return false;
        };

        if (try_existing(dir / (stem + ".key")))        break;
        if (try_existing(dir / (stem + "-key.pem")))    break;
        if (try_existing(dir / (stem + "_key.pem")))    break;
        if (try_existing(dir / "privkey.pem"))          break;
        if (try_existing(dir / "key.pem"))              break;
    }
}

// Scan certificate directory and discover cert/key pairs.
// Preference order: by certificate chain depth (more certs = fuller chain).
inline std::vector<ssl_cert_info> scan_cert_directory(const fs::path& cert_dir)
{
    std::map<std::string, std::vector<fs::path>> cert_candidates;
    std::map<std::string, std::vector<fs::path>> key_candidates;
    std::vector<fs::path> all_files;

    collect_certificate_files(cert_dir, cert_candidates, key_candidates, all_files);

    std::vector<ssl_cert_info> results;
    match_certs_with_keys(cert_candidates, key_candidates, results);

    if (results.empty())
        fallback_scan_cert_pairs(all_files, results);

    return results;
}

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
		{ ".txt", "text/plain; charset=utf-8" },
		{ ".md", "text/plain; charset=utf-8" },
		{ ".log", "text/plain; charset=utf-8" },
		{ ".xml", "text/xml" },
		{ ".ico", "image/x-icon" },
		{ ".ttf", "application/x-font-ttf" },
		{ ".eot", "application/vnd.ms-fontobject" },
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
		{ ".m4a", "audio/mp4" },
		{ ".mp3", "audio/mpeg" },
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
		{ ".webm", "video/webm" },
		{ ".weba", "audio/webm" },
		{ ".m3u8", "application/vnd.apple.mpegurl" }
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
		XLOG_ERR << "Open stdin error: " << ec.message();
		co_return;
	}

	XLOG_DBG << "Open stdin successfully";

	scoped_exit se([]()
		{
			global_quit = true;
			XLOG_DBG << "Quit read from stdin";
		});

	steady_timer timer(ex);

	while (true)
	{
		auto size = global_publish_subscribe.size();
		if (size == 0)
		{
			if (!is.is_open())
				break;

			timer.expires_after(std::chrono::milliseconds(100));
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
				XLOG_DBG << "No client connection";
				break;
			}

			if (ec)
				break;
		}
	}

	co_return;
}

template <typename Stream>
inline awaitable_void error_session(
	Stream& stream,
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
		XLOG_ERR << "Session: "
			<< connection_id
			<< ", async_write: "
			<< ec.message();
	}

	co_return;
}

template <typename Stream>
inline awaitable_void pipe_session(
	Stream& stream, dynamic_request& req, int64_t connection_id)
{
	XLOG_INFO << "Session: "
		<< connection_id
		<< ", pipe session started";

	boost::system::error_code ec;

	using buffer_queue_type = std::deque<publish_subscribe::data_type>;
	buffer_queue_type buffer_queue;

	auto ex = co_await net::this_coro::executor;
	steady_timer notify(ex);

	auto fetch_data =
		[&buffer_queue, &notify]
	(publish_subscribe::data_type data) mutable
	{
		net::dispatch(notify.get_executor(),
			[&buffer_queue, &notify, data]() mutable
			{
				buffer_queue.push_back(data);
				notify.cancel_one();
			});
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
		XLOG_ERR << "Session: "
			<< connection_id
			<< ", async_write_header: "
			<< ec.message();
		co_return;
	}

	int64_t total_bytes = 0;

	do
	{
		if (buffer_queue.empty())
		{
			if (file_size == 0)
				break;

			notify.expires_after(std::chrono::seconds(60));
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
			total_bytes += p->size();
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
			XLOG_ERR << "Session: "
				<< connection_id
				<< ", async_write body: "
				<< ec.message();
			co_return;
		}
	} while (!sr.is_done());

	XLOG_INFO << "Session: "
		<< connection_id
		<< ", pipe session finished, total bytes: "
		<< total_bytes;

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

// Truncate a display name to at most 50 characters, appending "..&gt;" when cut.
inline std::wstring truncate_display_name(std::wstring name)
{
	if (name.size() > 50)
	{
		name.resize(47);
		name += L"..&gt;";
	}
	return name;
}

// Format a single filesystem entry (dir or file) as an HTML list row.
inline std::wstring format_list_entry(
	const fs::path& item,
	const std::wstring& rpath,
	const std::wstring& time_string,
	const std::wstring& size_string)
{
	int width = 50 - static_cast<int>(rpath.size());
	if (width < 0) width = 0;

	return fmt::format(body_fmt,
		rpath,
		truncate_display_name(rpath),
		std::wstring(static_cast<std::size_t>(width), L' '),
		time_string,
		size_string);
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

		if (fs::is_directory(item, ec))
		{
			auto leaf = boost::nowide::narrow(item.filename().wstring());
			leaf += "/";
			auto rpath = boost::nowide::widen(leaf);

			path_list.push_back(format_list_entry(
				item, rpath, time_string, L"-"));
		}
		else
		{
			auto rpath = boost::nowide::widen(
				boost::nowide::narrow(item.filename().wstring()));

			if (unc_path.empty())
				unc_path = item;
			auto sz = static_cast<float>(fs::file_size(unc_path, ec));
			if (ec)
				sz = 0;
			auto filesize = boost::nowide::widen(add_suffix(sz));

			path_list.push_back(format_list_entry(
				item, rpath, time_string, filesize));
		}
	}

	return path_list;
}

inline std::map<std::string, std::string> parse_query_string(std::string_view qs)
{
	std::map<std::string, std::string> params;
	if (qs.empty())
		return params;

	std::string_view::size_type start = 0;
	while (start < qs.size())
	{
		auto amp = qs.find('&', start);
		auto eq = qs.find('=', start);
		auto end = (amp == std::string_view::npos) ? qs.size() : amp;

		if (eq != std::string_view::npos && eq < end)
		{
			auto key = qs.substr(start, eq - start);
			auto value = qs.substr(eq + 1, end - eq - 1);
			params[std::string(key)] = std::string(value);
		}
		else
		{
			auto key = qs.substr(start, end - start);
			params[std::string(key)] = "";
		}

		if (amp == std::string_view::npos)
			break;
		start = amp + 1;
	}

	return params;
}

// Build an HTML directory listing body from the dirs/files sets and root path.
inline std::string build_directory_listing_body(
    const fs::path& dir,
    const std::set<fs::path>& dirs,
    const std::set<fs::path>& files)
{
    std::vector<std::wstring> path_list = format_path_list(dirs);
    auto file_list = format_path_list(files);
    path_list.insert(path_list.end(), file_list.begin(), file_list.end());

    auto current_dir = dir.wstring();
    auto root_path = boost::replace_first_copy(
        current_dir, global_path.wstring(), L"");

    std::wstring body = fmt::format(
        body_fmt, L"../", L"../", L"", L"", L"");

    for (auto& s : path_list)
        body += s;

    return boost::nowide::narrow(
        fmt::format(head_fmt, root_path, root_path) + body + tail_fmt);
}

// Scan a directory into two sorted sets: subdirectories and files.
inline bool scan_directory_entries(
    const fs::path& dir,
    std::set<fs::path>& dirs,
    std::set<fs::path>& files,
    boost::system::error_code& ec)
{
    fs::directory_iterator end;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return false;

    for (; it != end; it++)
    {
        const auto& item = it->path();
        if (fs::is_directory(item, ec))
            dirs.insert(item);
        else
            files.insert(item);
    }
    return true;
}

// Helper: send a string response with standard headers and serialize it.
template <typename Stream>
inline awaitable_void send_string_response(
    Stream& stream,
    dynamic_request& req,
    int64_t connection_id,
    http::status status,
    std::string body,
    const std::string& content_type)
{
    boost::system::error_code ec;

    string_response res{status, req.version()};
    res.set(http::field::server, version_string);
    res.set(http::field::date, server_date_string());
    res.set(http::field::content_type, content_type);
    res.keep_alive(req.keep_alive());
    res.content_length(body.size());
    res.body() = std::move(body);
    res.prepare_payload();

    http::serializer<false, string_body, http::fields> sr(res);
    co_await http::async_write(stream, sr, ioc_awaitable[ec]);

    if (ec)
        XLOG_ERR << "Session: " << connection_id << ", err: " << ec.message();
    co_return;
}

template <typename Stream>
inline awaitable_void dir_session(
	Stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	fs::path dir)
{
	XLOG_DBG << "Session: " << connection_id << ", path: " << dir.string();

	boost::system::error_code ec;

	std::set<fs::path> dirs;
	std::set<fs::path> files;

	if (!scan_directory_entries(dir, dirs, files, ec))
	{
		XLOG_WARN << "Session: " << connection_id
			<< ", path: " << dir.string() << ", err: " << ec.message();

		co_await error_session(stream, req, connection_id,
			http::status::internal_server_error, "Internal server error");
		co_return;
	}

	auto body = build_directory_listing_body(dir, dirs, files);

	co_await send_string_response(
		stream, req, connection_id,
		http::status::ok, std::move(body),
		"text/html; charset=UTF-8");

	co_return;
}

// Build a JSON array string from directory contents.
inline std::string build_directory_listing_json(
    const fs::path& dir,
    boost::system::error_code& ec)
{
    fs::directory_iterator end;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return {};

    std::string json = "[\n";
    bool first = true;

    for (; it != end; it++)
    {
        const auto& item = it->path();

        if (!first)
            json += ",\n";
        first = false;

        auto filename = boost::nowide::narrow(item.filename().wstring());
        auto [ftime, unc_path] = file_last_wirte_time(item);

        if (fs::is_directory(item, ec))
        {
            json += fmt::format(
                R"(  {{"last_write_time": "{}", "filename": "{}", "is_dir": true}})",
                ftime, filename);
        }
        else
        {
            auto sz = static_cast<float>(fs::file_size(item, ec));
            if (ec)
                sz = 0;
            json += fmt::format(
                R"(  {{"last_write_time": "{}", "filename": "{}", "is_dir": false, "filesize": {}}})",
                ftime, filename, static_cast<int64_t>(sz));
        }
    }

    json += "\n]\n";
    return json;
}

template <typename Stream>
inline awaitable_void dir_session_json(
	Stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	fs::path dir)
{
	XLOG_DBG << "Session: "
		<< connection_id
		<< ", path: "
		<< dir.string()
		<< ", json listing";

	boost::system::error_code ec;
	auto json = build_directory_listing_json(dir, ec);

	if (ec)
	{
		XLOG_WARN << "Session: " << connection_id
			<< ", path: " << dir.string() << ", err: " << ec.message();

		co_await error_session(stream, req, connection_id,
			http::status::internal_server_error, "Internal server error");
		co_return;
	}

	co_await send_string_response(
		stream, req, connection_id,
		http::status::ok, std::move(json),
		"application/json; charset=utf-8");

	co_return;
}

// Stream file body to the client using buffer_body serialization.
template <typename Stream>
inline awaitable_void stream_file_body(
    Stream& stream,
    buffer_response& res,
    response_serializer& sr,
    std::fstream& file_stream,
    size_t content_length,
    int64_t connection_id)
{
    boost::system::error_code ec;

    res.body().data = nullptr;
    res.body().more = false;

    co_await http::async_write_header(stream, sr, ioc_awaitable[ec]);
    if (ec)
    {
        XLOG_WARN << "Session: " << connection_id
            << ", async_write_header: " << ec.message();
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
            static_cast<std::streamsize>(content_length) - total);

        if (bytes_transferred == 0 ||
            total >= static_cast<std::streamsize>(content_length))
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

        co_await http::async_write(stream, sr, ioc_awaitable[ec]);
        total += bytes_transferred;

        if (ec == http::error::need_buffer)
        {
            ec = {};
            continue;
        }
        if (ec)
        {
            XLOG_WARN << "Session: " << connection_id
                << ", async_write: " << ec.message();
            co_return;
        }
    } while (!sr.is_done());

    co_return;
}

// Calculate range boundaries for a partial content request.
// Returns adjusted first/last positions and whether the range is valid.
struct range_info
{
    int64_t first{0};
    int64_t last{-1};
    bool valid{false};
};

inline range_info resolve_range(
    const ranges& range_list,
    size_t content_length,
    int64_t connection_id,
    std::iostream& file_stream)
{
    range_info info;
    if (range_list.empty())
    {
        info.valid = true;
        info.first = 0;
        info.last = static_cast<int64_t>(content_length) - 1;
        return info;
    }

    auto r = range_list.front();

    if (r.second == -1)
    {
        if (r.first < 0)
        {
            r.first = static_cast<int64_t>(content_length) + r.first;
            r.second = static_cast<int64_t>(content_length) - 1;
        }
        else if (r.first >= 0)
        {
            r.second = static_cast<int64_t>(content_length) - 1;
        }
    }

    if (r.second < r.first && r.second >= 0)
    {
        XLOG_WARN << "Session: " << connection_id
            << ", invalid range: " << r.first << "-" << r.second;
        return info;
    }

    file_stream.seekg(r.first, std::ios_base::beg);
    info.first = r.first;
    info.last = r.second;
    info.valid = true;
    return info;
}

// Select the MIME content type from the global map, defaulting to text/plain.
inline std::string select_content_type(const fs::path& file)
{
    auto ext = strutil::to_lower(file.extension().string());
    auto it = global_mimes.find(ext);
    if (it != global_mimes.end())
        return it->second;
    return "text/plain; charset=utf-8";
}

template <typename Stream>
inline awaitable_void file_session(
	Stream& stream,
	dynamic_request& req,
	int64_t connection_id,
	fs::path file)
{
	XLOG_DBG << "Session: " << connection_id << ", file: " << file.string();

	if (req.method() != http::verb::get)
	{
		co_await error_session(stream, req, connection_id,
			http::status::bad_request, "Bad request");
		co_return;
	}

#ifdef WIN32
	auto filename = file.wstring();
	boost::replace_all(filename, "/", "\\");
	filename = L"\\\\?\\" + filename;
	file = filename;
#endif

	if (!fs::exists(file))
	{
		co_await error_session(stream, req, connection_id,
			http::status::not_found, "Not Found");
		co_return;
	}

	boost::system::error_code ec;
	size_t content_length = fs::file_size(file, ec);

	std::fstream file_stream(
		file.string(),
		std::ios_base::binary |
		std::ios_base::in);

	auto range = get_ranges(req["Range"]);
	http::status st = range.empty() ? http::status::ok
	                                : http::status::partial_content;

	buffer_response res{ st, req.version() };
	res.set(http::field::server, "httpd/1.0");
	res.set(http::field::content_type, select_content_type(file));

	if (!range.empty())
	{
		auto info = resolve_range(range, content_length, connection_id, file_stream);

		if (!info.valid)
		{
			co_await error_session(stream, req, connection_id,
				http::status::range_not_satisfiable, satisfiable_html);
			co_return;
		}

		std::string content_range = fmt::format(
			"bytes {}-{}/{}", info.first, info.last, content_length);
		content_length = info.last - info.first + 1;
		res.set(http::field::content_range, content_range);

		XLOG_DBG << "Session: " << connection_id
			<< ", range request: " << content_range;
	}
	else
	{
		res.set(http::field::accept_ranges, "bytes");
	}

	res.keep_alive(req.keep_alive());
	res.content_length(content_length);

	XLOG_INFO << "Session: " << connection_id
		<< ", serving file: " << file.filename().string()
		<< ", size: " << strutil::add_suffix(static_cast<float>(content_length));

	response_serializer sr(res);

	co_await stream_file_body(
		stream, res, sr, file_stream, content_length, connection_id);

	co_return;
}

// Format remote endpoint as a string suitable for logging.
inline std::string format_remote_host(tcp::endpoint endp)
{
    if (endp.address().is_v6())
        return "[" + endp.address().to_string() + "]:" + std::to_string(endp.port());
    return endp.address().to_string() + ":" + std::to_string(endp.port());
}

// Handle HTTP 100-continue expectation.
template <typename Stream>
inline awaitable_void handle_100_continue(
    Stream& stream,
    dynamic_request& req_ref,
    int64_t connection_id)
{
    boost::system::error_code ec;
    http::response<http::empty_body> res;
    res.version(11);
    res.result(http::status::continue_);

    co_await http::async_write(stream, res, ioc_awaitable[ec]);
    if (ec)
    {
        XLOG_ERR << "Session: " << connection_id
            << ", expect async_write: " << ec.message();
    }
    co_return;
}

// Resolve the request target against global_path and validate it
// (path traversal protection). On success, returns the canonical path.
// On failure, returns an empty path and sets ec.
inline fs::path resolve_request_path(
    const std::string& target,
    boost::system::error_code& ec)
{
    std::string unescaped;
    strutil::unescape({target.data(), target.size()}, unescaped);
    if (!unescaped.empty() && unescaped[0] == '/')
        unescaped.erase(0, 1);

    // Split off query string.
    auto qpos = unescaped.find('?');
    std::string path_part = unescaped.substr(0, qpos);

    auto current_path = fs::canonical(
        global_path / boost::nowide::widen(path_part), ec).make_preferred();

    if (ec || !current_path.wstring().starts_with(global_path.wstring()))
    {
        ec = boost::asio::error::not_found;
        return {};
    }

    return current_path;
}

// Extract query string from a raw target string.
inline std::string extract_query_string(const std::string& target)
{
    auto qpos = target.find('?');
    if (qpos != std::string::npos)
        return target.substr(qpos + 1);
    return {};
}

template <typename Stream>
inline awaitable_void session(Stream stream)
{
	static int64_t static_connection_id = 0;
	static size_t num_connections = 0;

	int64_t connection_id = static_connection_id++;
	num_connections++;

	boost::system::error_code ec;

	std::string remote_host;
	auto endp = beast::get_lowest_layer(stream).socket().remote_endpoint(ec);
	if (!ec)
		remote_host = format_remote_host(endp);

	XLOG_DBG << "Session: "
		<< connection_id
		<< ", host: "
		<< remote_host
		<< " is coming...";

	scoped_exit se_quit([&]()
		{
			num_connections--;

			XLOG_DBG << "Session: "
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

		// 设置请求体和请求头的限制，防止过大的请求导致内存耗尽或拒绝服务攻击。
		parser.body_limit(std::numeric_limits<uint64_t>::max());
		parser.header_limit(static_cast<uint32_t>(global_buffer_size >> 2));

		// 读取 HTTP 请求头.
		co_await http::async_read_header(stream,
			buffer,
			parser,
			ioc_awaitable[ec]);
		if (ec)
		{
			XLOG_WARN << "Session: "
				<< connection_id
				<< ", async_read_header: "
				<< ec.message();
			co_return;
		}

		auto& req_ref = parser.get();
		XLOG_INFO << "Session: "
			<< connection_id
			<< ", "
			<< std::string(req_ref.method_string())
			<< " "
			<< std::string(req_ref.target())
			<< ", host: "
			<< remote_host;

		if (req_ref[http::field::expect] == "100-continue")
		{
			co_await handle_100_continue(stream, req_ref, connection_id);
			if (ec)
				co_return;
		}

		// Git LFS 路由处理：在释放 parser 之前检查，以便读取请求体.
		if (!global_lfs_storage_dir.empty())
		{
			auto target_str = req_ref.target();
			auto method = req_ref.method();

			if (target_str == "/objects/batch" && method == http::verb::post)
			{
				XLOG_INFO << "Session: " << connection_id << ", LFS batch request";
				co_await http::async_read(stream, buffer, parser, ioc_awaitable[ec]);
				if (ec)
					co_return;
				{
					auto req = parser.release();
					co_await lfs_batch_session(stream, req, connection_id,
						std::is_same_v<std::decay_t<Stream>, ssl_stream>);
				}
				co_return;
			}

			if (target_str.starts_with("/files/") && method == http::verb::put)
			{
				auto oid = target_str.substr(7);
				if (!oid.empty())
				{
					XLOG_INFO << "Session: " << connection_id
						<< ", LFS file upload, oid: " << std::string(oid);
					co_await lfs_file_upload_session(
						stream, buffer, parser, connection_id, oid);
				}
				co_return;
			}

			if (target_str.starts_with("/files/") && method == http::verb::get)
			{
				auto oid = target_str.substr(7);
				if (!oid.empty())
				{
					XLOG_INFO << "Session: " << connection_id
						<< ", LFS file download, oid: " << std::string(oid);
					auto req = parser.release();
					co_await lfs_file_download_session(stream, req, connection_id, oid);
				}
				co_return;
			}
		}

		dynamic_request req = parser.release();

		if (beast::websocket::is_upgrade(req))
			co_return;

		if (global_pipe)
		{
			co_await pipe_session(stream, req, connection_id);
			co_return;
		}

		if (fs::is_regular_file(global_path))
		{
			co_await file_session(stream, req, connection_id, global_path);
			if (keep_alive)
				continue;
			co_return;
		}

		if (!fs::is_directory(global_path))
		{
			co_await error_session(stream, req, connection_id,
				http::status::internal_server_error, "internal server error");
			if (keep_alive)
				continue;
			co_return;
		}

		keep_alive = req.keep_alive();

		auto current_path = resolve_request_path(
			std::string(req.target()), ec);

		if (ec || current_path.empty())
		{
			co_await error_session(stream, req, connection_id,
				http::status::not_found, "Not Found");

			XLOG_WARN << "Session: "
				<< connection_id
				<< ", path traversal blocked: "
				<< std::string(req.target());

			if (keep_alive)
				continue;
			co_return;
		}

		auto realpath = addLongPathAware(current_path);

		if (fs::is_directory(realpath))
		{
			// 如果请求带有 ?q=json 查询参数, 则返回 JSON 格式的目录列表.
			auto query_string = extract_query_string(
				std::string(req.target()));
			auto query_params = parse_query_string(query_string);
			if (auto qit = query_params.find("q");
				qit != query_params.end() && qit->second == "json")
			{
				XLOG_DBG << "Session: " << connection_id
					<< ", directory JSON listing for: " << current_path;

				co_await dir_session_json(stream, req, connection_id, current_path);
				if (keep_alive)
					continue;
				co_return;
			}

			// 如果目录下有 index.html 或 index.htm，则直接返回该文件.
			boost::system::error_code index_ec;
			auto index_path = current_path / "index.html";
			if (!fs::exists(index_path, index_ec))
				index_path = current_path / "index.htm";

			if (!index_ec && fs::exists(index_path, index_ec))
			{
				XLOG_DBG << "Session: " << connection_id
					<< ", serving index file: " << index_path;

				co_await file_session(stream, req, connection_id, index_path);
				if (keep_alive)
					continue;
				co_return;
			}

			XLOG_DBG << "Session: " << connection_id
				<< ", directory listing for: " << current_path;

			co_await dir_session(stream, req, connection_id, current_path);
			if (keep_alive)
				continue;
			co_return;
		}

		if (fs::is_regular_file(realpath))
		{
			XLOG_DBG << "Session: " << connection_id
				<< ", serving file: " << current_path;

			co_await file_session(stream, req, connection_id, current_path);
			if (keep_alive)
				continue;
			co_return;
		}

		if (!fs::exists(realpath))
		{
			co_await error_session(stream, req, connection_id,
				http::status::not_found, "Not Found");

			XLOG_WARN << "Session: " << connection_id
				<< ", path not found: " << current_path;

			if (keep_alive)
				continue;
			co_return;
		}

		co_await error_session(stream, req, connection_id,
			http::status::bad_request, "Bad request");

		XLOG_WARN << "Session: " << connection_id
			<< ", bad request for: " << current_path;

		if (keep_alive)
			continue;
		co_return;
	}

	co_return;
}

inline awaitable_void listen(tcp_acceptor& acceptor)
{
	auto local_endpoint = acceptor.local_endpoint();
	XLOG_INFO << "HTTP listening on "
		<< net::ip::tcp::endpoint(local_endpoint);

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

		XLOG_DBG << "New connection accepted, spawning session...";

		net::co_spawn(
			ex,
			session(std::move(stream)),
			net::detached);
	}

	co_return;
}

inline awaitable_void ssl_listen(
	tcp_acceptor& acceptor, ssl::context& ctx)
{
	auto local_endpoint = acceptor.local_endpoint();
	XLOG_INFO << "HTTPS listening on "
		<< net::ip::tcp::endpoint(local_endpoint);

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
		ssl_stream stream(std::move(client), ctx);

		// SSL handshake.
		co_await stream.async_handshake(
			ssl::stream_base::server,
			ioc_awaitable[ec]);
		if (ec)
		{
			XLOG_ERR << "SSL handshake failed: "
				<< ec.message();
			continue;
		}

		XLOG_DBG << "SSL handshake successful, spawning session...";

		net::co_spawn(
			ex,
			session(std::move(stream)),
			net::detached);
	}

	co_return;
}


// Set up the global SSL context from certificates discovered in cert_dir.
// Returns true on success, false on failure (with log).
inline bool setup_ssl_context(const std::string& httpd_ssl_cert_dir)
{
    auto cert_dir = fs::path(httpd_ssl_cert_dir).make_preferred();
    XLOG_INFO << "Scanning SSL certificate directory: " << cert_dir.string();

    if (!fs::is_directory(cert_dir))
    {
        XLOG_ERR << "SSL cert directory not found: " << cert_dir.string();
        return false;
    }

    auto certs = scan_cert_directory(cert_dir);
    if (certs.empty())
    {
        XLOG_ERR << "No valid SSL certificates found in: " << cert_dir.string();
        return false;
    }

    XLOG_INFO << "Found " << certs.size()
        << " certificate(s) in: " << cert_dir.string();

    global_ssl_ctx = std::make_shared<ssl::context>(
        ssl::context::tls_server);

    for (const auto& info : certs)
    {
        boost::system::error_code ec;

        global_ssl_ctx->use_certificate_chain_file(
            info.cert_file.string(), ec);
        if (ec)
        {
            XLOG_ERR << "Failed to load cert: "
                << info.cert_file.string() << ", err: " << ec.message();
            continue;
        }

        global_ssl_ctx->use_private_key_file(
            info.key_file.string(),
            ssl::context::pem, ec);
        if (ec)
        {
            XLOG_ERR << "Failed to load key: "
                << info.key_file.string() << ", err: " << ec.message();
            continue;
        }

        XLOG_INFO << "SSL certificate loaded: "
            << info.cert_file.filename().string()
            << " (domain: " << info.domain << ")";
        break; // Only use the first valid certificate.
    }

    global_ssl_ctx->set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2 |
        ssl::context::no_sslv3 |
        ssl::context::single_dh_use);

    return true;
}

// Parse the listen address string into host, port, and v6only flag.
inline bool parse_listen_address(
    const std::string& httpd_listen,
    tcp::endpoint& endpoint)
{
    std::string host;
    std::string port;
    bool v6only;

    if (!parse_endpoint_string(httpd_listen, host, port, v6only))
        return false;

    endpoint = tcp::endpoint(
        net::ip::make_address(host),
        static_cast<unsigned short>(std::stoi(port)));
    return true;
}

// Spawn N acceptor coroutines on the given executor.
inline void spawn_acceptor(
    net::io_context::executor_type ex,
    tcp_acceptor& acceptor,
    ssl::context* ssl_ctx)
{
    int count = 16;

    if (ssl_ctx)
    {
        XLOG_INFO << "Spawning 16 SSL accept coroutines...";
        for (int i = 0; i < count; i++)
            net::co_spawn(ex, ssl_listen(acceptor, *ssl_ctx), net::detached);
    }
    else
    {
        XLOG_INFO << "Spawning 16 accept coroutines...";
        for (int i = 0; i < count; i++)
            net::co_spawn(ex, listen(acceptor), net::detached);
    }
}

int main(int argc, char** argv)
{
	platform_init();

	std::string httpd_listen;
	std::string httpd_doc;
	std::string httpd_ssl_cert_dir;
	std::string httpd_lfs_storage_dir;
	std::string httpd_log_dir;

	// 解析命令行.
	po::options_description desc("Options");
	desc.add_options()
		("help,h", "Help message.")
		("listen", po::value<std::string>(&httpd_listen)->default_value("[::0]:80")->value_name("ip:port"), "Listen address (e.g. [::0]:80, 0.0.0.0:8080). When --ssl-cert-dir is set, serves HTTPS.")
		("path", po::value<std::string>(&httpd_doc)->value_name("path"), "Document root directory, a single file to serve, or '-'/empty for stdin pipe mode.")
		("ssl-cert-dir", po::value<std::string>(&httpd_ssl_cert_dir)->value_name("dir"), "SSL certificate directory. Enables HTTPS when specified.")
		("lfs-storage-dir", po::value<std::string>(&httpd_lfs_storage_dir)->value_name("dir"), "Git LFS storage directory. Enables LFS batch API and file transfer endpoints when specified.")
		("log-dir", po::value<std::string>(&httpd_log_dir)->value_name("dir"), "Log file output directory.")
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

	// 初始化日志系统，可指定日志输出目录.
	if (vm.count("log-dir"))
		xlogger::init_logging(httpd_log_dir);

	// SSL 证书目录处理.
	if (vm.count("ssl-cert-dir") && !setup_ssl_context(httpd_ssl_cert_dir))
		return EXIT_FAILURE;

	tcp::endpoint listen_endpoint;

	// 解析侦听端口.
	if (!parse_listen_address(httpd_listen, listen_endpoint))
	{
		std::cerr << "Cannot parse listen: " << httpd_listen << "\n";
		return EXIT_FAILURE;
	}

	net::io_context ctx;
	tcp_acceptor acceptor(ctx, listen_endpoint);

	XLOG_INFO << "Starting httpd server..."
		<< " listen=" << httpd_listen
		<< " mode=" << (global_ssl_ctx ? "HTTPS" : "HTTP");

	if (vm.count("path") && !httpd_doc.empty() && httpd_doc != "-")
	{
		global_path = fs::canonical(fs::path(httpd_doc).make_preferred());
		XLOG_INFO << "Document root: " << global_path.string();
	}

	if (vm.count("log-dir"))
	{
		XLOG_INFO << "Log directory: " << httpd_log_dir
			<< " (log file: " << xlogger::log_path() << ")";
	}

	// 启动tcp侦听.
	spawn_acceptor(ctx.get_executor(), acceptor, global_ssl_ctx.get());

	// 如果是pipe, 则直接启动文件读.
	if (httpd_doc.empty() || httpd_doc == "-")
	{
		global_pipe = true;

		XLOG_INFO << "Pipe mode enabled, reading from stdin...";

		net::co_spawn(
			ctx.get_executor(),
			read_from_stdin(),
			net::detached);
	}

	// Git LFS 存储目录.
	if (vm.count("lfs-storage-dir"))
	{
		global_lfs_storage_dir = fs::path(httpd_lfs_storage_dir).make_preferred();
		boost::system::error_code lfs_ec;
		fs::create_directories(global_lfs_storage_dir, lfs_ec);
		if (lfs_ec)
		{
			XLOG_ERR << "Failed to create LFS storage directory: "
				<< global_lfs_storage_dir << ", err: " << lfs_ec.message();
			return EXIT_FAILURE;
		}
		XLOG_INFO << "Git LFS storage directory: " << global_lfs_storage_dir.string();
	}

	XLOG_INFO << "httpd server started, entering event loop...";

	ctx.run();

	XLOG_INFO << "httpd server stopped.";

	return EXIT_SUCCESS;
}
