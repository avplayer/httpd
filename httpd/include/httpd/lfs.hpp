//
// lfs.hpp
// ~~~~~~~~
//
// Copyright (c) 2022 Jack (jack.arain at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Git LFS 协议支持服务端实现。
// 协议参考: https://github.com/git-lfs/git-lfs/blob/main/docs/api/batch.md
//

#ifndef INCLUDE__2024_01_15__LFS_HPP
#define INCLUDE__2024_01_15__LFS_HPP

#include <string>
#include <string_view>
#include <fstream>
#include <cstdint>
#include <vector>
#include <memory>

#include <openssl/evp.h>

#include <boost/beast/core.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http.hpp>

namespace beast = boost::beast;
namespace http = beast::http;

#include <boost/asio/io_context.hpp>
namespace net = boost::asio;

#include <boost/json.hpp>
namespace json = boost::json;

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#ifdef USE_STD_FILESYSTEM
# include <filesystem>
namespace fs = std::filesystem;
#endif

#include "httpd/use_awaitable.hpp"
#include "httpd/logging.hpp"
#include "httpd/scoped_exit.hpp"

// 使用与主程序相同的 executor 类型.
using executor_type = net::io_context::executor_type;
using awaitable_void = net::awaitable<void, executor_type>;
using awaitable_string = net::awaitable<std::string, executor_type>;

// LFS 存储根目录，默认空表示 LFS 功能未启用.
inline fs::path global_lfs_storage_dir;

//////////////////////////////////////////////////////////////////////////
// LFS 协议 JSON 结构体构建与解析
//////////////////////////////////////////////////////////////////////////

namespace lfs_detail {

// 构建批处理响应 JSON.
inline json::value build_batch_response(
    const json::value& req_body,
    const std::string& operation,
    const fs::path& storage_dir,
    const std::string& server_url)
{
    json::array objects_arr;

    if (req_body.kind() != json::kind::object)
        return json::object{ {"transfer", "basic"}, {"objects", json::array()} };

    const auto& req_obj = req_body.get_object();

    // 获取 objects 数组.
    auto it = req_obj.find("objects");
    if (it == req_obj.end() || it->value().kind() != json::kind::array)
        return json::object{ {"transfer", "basic"}, {"objects", json::array()} };

    const auto& objects = it->value().get_array();

    for (const auto& obj : objects)
    {
        if (obj.kind() != json::kind::object)
            continue;

        const auto& obj_obj = obj.get_object();

        auto oid_it = obj_obj.find("oid");
        auto size_it = obj_obj.find("size");

        if (oid_it == obj_obj.end() || oid_it->value().kind() != json::kind::string)
            continue;

        std::string oid = oid_it->value().get_string().c_str();
        int64_t size = 0;
        if (size_it != obj_obj.end() && size_it->value().kind() == json::kind::int64)
            size = size_it->value().get_int64();

        json::object obj_resp;
        obj_resp["oid"] = oid;
        obj_resp["size"] = size;

        if (operation == "upload")
        {
            // 如果文件已存在，则跳过，避免重复上传.
            fs::path file_path = storage_dir / oid;
            boost::system::error_code ec;
            if (fs::exists(file_path, ec))
                continue;

            json::object upload_action;
            upload_action["href"] = server_url + "/files/" + oid;
            json::object actions;
            actions["upload"] = upload_action;
            obj_resp["actions"] = actions;
        }
        else if (operation == "download")
        {
            // 校验本地文件是否存在.
            fs::path file_path = storage_dir / oid;
            boost::system::error_code ec;
            if (fs::exists(file_path, ec))
            {
                json::object download_action;
                download_action["href"] = server_url + "/files/" + oid;
                json::object actions;
                actions["download"] = download_action;
                obj_resp["actions"] = actions;
            }
        }

        objects_arr.push_back(obj_resp);
    }

    json::object response;
    response["transfer"] = "basic";
    response["objects"] = objects_arr;
    return response;
}

// 验证 OID 是否为有效的 SHA-256 哈希值（64 个十六进制字符）.
inline bool is_valid_oid(std::string_view oid)
{
    if (oid.size() != 64)
        return false;
    for (char c : oid)
    {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

} // namespace lfs_detail

//////////////////////////////////////////////////////////////////////////
// LFS 错误响应辅助函数 — 统一处理 LFS 协议错误响应，消除重复代码
//////////////////////////////////////////////////////////////////////////

template <typename Stream, typename Request>
inline awaitable_void lfs_error_response(
    Stream& stream,
    Request& req,
    http::status code,
    const std::string& message)
{
    http::response<http::string_body> res{code, req.version()};
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/vnd.git-lfs+json");
    res.keep_alive(req.keep_alive());
    res.body() = message;
    res.prepare_payload();

    boost::system::error_code ec;
    co_await http::async_write(stream, res, ioc_awaitable[ec]);
    co_return;
}

//////////////////////////////////////////////////////////////////////////
// SHA-256 工具函数 — 将 EVP_MD_CTX 的最终哈希值转换为十六进制字符串
//////////////////////////////////////////////////////////////////////////

inline std::string sha256_to_hex_string(EVP_MD_CTX* ctx)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);

    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int i = 0; i < hash_len; ++i)
    {
        result += hex_chars[(hash[i] >> 4) & 0x0F];
        result += hex_chars[hash[i] & 0x0F];
    }
    return result;
}

//////////////////////////////////////////////////////////////////////////
// LFS 批处理请求处理 (POST /objects/batch)
//////////////////////////////////////////////////////////////////////////

// Result of parsing an LFS batch request.
struct lfs_batch_request_info
{
    json::value req_json;
    std::string operation;
    bool valid{false};
};

// Parse the LFS batch request body (pure parsing, no I/O).
inline lfs_batch_request_info parse_lfs_batch_request(
    http::request<http::dynamic_body>& req,
    int64_t connection_id)
{
    lfs_batch_request_info result;

    std::string body_str = beast::buffers_to_string(req.body().data());

    try
    {
        result.req_json = json::parse(body_str);
    }
    catch (const std::exception& e)
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", invalid JSON: " << e.what();
        return result;
    }

    if (result.req_json.kind() == json::kind::object)
    {
        auto op_it = result.req_json.get_object().find("operation");
        if (op_it != result.req_json.get_object().end() &&
            op_it->value().kind() == json::kind::string)
        {
            result.operation = op_it->value().get_string().c_str();
        }
    }

    if (result.operation != "upload" && result.operation != "download")
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", invalid operation: " << result.operation;
        return result;
    }

    result.valid = true;
    return result;
}

// Build the server URL from the Host header and SSL flag.
inline std::string build_lfs_server_url(
    http::request<http::dynamic_body>& req,
    bool is_ssl)
{
    std::string url = is_ssl ? "https://" : "http://";
    auto host_it = req.find(http::field::host);
    if (host_it != req.end())
        url += host_it->value();
    else
        url += "localhost:8080";
    return url;
}

template <typename Stream>
inline awaitable_void lfs_batch_session(
    Stream& stream,
    http::request<http::dynamic_body>& req,
    int64_t connection_id,
    bool is_ssl = false)
{
    boost::system::error_code ec;

    XLOG_DBG << "LFS Session: " << connection_id << ", batch request";

    auto info = parse_lfs_batch_request(req, connection_id);
    if (!info.valid)
    {
        std::string err_msg = info.operation.empty()
            ? R"({"message":"Invalid JSON"})"
            : R"({"message":"Invalid operation"})";
        co_await lfs_error_response(stream, req,
            http::status::bad_request, err_msg);
        co_return;
    }

    auto server_url = build_lfs_server_url(req, is_ssl);
    auto resp_json = lfs_detail::build_batch_response(
        info.req_json, info.operation, global_lfs_storage_dir, server_url);

    std::string resp_body = json::serialize(resp_json);

    http::response<http::string_body> res{
        http::status::ok, req.version()
    };
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/vnd.git-lfs+json");
    res.keep_alive(req.keep_alive());
    res.content_length(resp_body.size());
    res.body() = std::move(resp_body);
    res.prepare_payload();

    co_await http::async_write(stream, res, ioc_awaitable[ec]);

    if (ec)
        XLOG_ERR << "LFS Session: " << connection_id
            << ", batch write error: " << ec.message();

    co_return;
}

//////////////////////////////////////////////////////////////////////////
// LFS 文件上传处理 (PUT /files/{oid})
//////////////////////////////////////////////////////////////////////////

// Stream the request body into a file while computing SHA-256.
// Returns the computed hex OID, or an empty string on failure.
// On error, the partially written file is removed and an error is sent.
template <typename Stream>
inline awaitable_string stream_upload_to_file(
    Stream& stream,
    beast::flat_buffer& buffer,
    http::request_parser<http::request<http::dynamic_body>::body_type>& parser,
    int64_t connection_id,
    const fs::path& file_path,
    std::ofstream& file_stream,
    EVP_MD_CTX* md_ctx)
{
    boost::system::error_code ec;

    while (!parser.is_done())
    {
        co_await http::async_read_some(stream, buffer, parser, ioc_awaitable[ec]);

        auto& body = parser.get().body();
        auto bodysize = body.size();

        if (bodysize > 0)
        {
            for (auto const buf : boost::beast::buffers_range(body.data()))
            {
                auto bufsize = buf.size();
                auto bufptr = static_cast<const char*>(buf.data());

                file_stream.write(bufptr, bufsize);

                if (!file_stream)
                {
                    XLOG_ERR << "LFS Session: " << connection_id
                        << ", file write failed";
                    file_stream.close();
                    boost::system::error_code remove_ec;
                    fs::remove(file_path, remove_ec);
                    co_return "";
                }

                EVP_DigestUpdate(md_ctx, bufptr, bufsize);
            }

            body.consume(bodysize);
            continue;
        }

        if (ec == http::error::need_buffer)
        {
            ec = {};
            continue;
        }
        if (ec)
        {
            XLOG_ERR << "LFS Session: " << connection_id
                << ", upload read error: " << ec.message();
            file_stream.close();
            boost::system::error_code remove_ec;
            fs::remove(file_path, remove_ec);
            co_return "";
        }
    }

    co_return sha256_to_hex_string(md_ctx);
}

template <typename Stream>
inline awaitable_void lfs_file_upload_session(
    Stream& stream,
    beast::flat_buffer& buffer,
    http::request_parser<http::request<http::dynamic_body>::body_type>& parser,
    int64_t connection_id,
    std::string_view oid)
{
    auto& req = parser.get();

    XLOG_DBG << "LFS Session: " << connection_id
        << ", file upload (streaming), oid: " << oid;

    if (!lfs_detail::is_valid_oid(oid))
    {
        co_await lfs_error_response(stream, req,
            http::status::bad_request, R"({"message":"Invalid OID"})");
        co_return;
    }

    fs::path file_path = global_lfs_storage_dir / std::string(oid);

    XLOG_DBG << "LFS Session: " << connection_id
        << ", uploading file: " << file_path.string();

    boost::system::error_code ec;
    fs::create_directories(global_lfs_storage_dir, ec);

    std::ofstream file_stream(
        file_path.string(),
        std::ios_base::binary | std::ios_base::trunc);

    if (!file_stream.is_open())
    {
        co_await lfs_error_response(stream, req,
            http::status::internal_server_error,
            R"({"message":"Internal Server Error"})");
        co_return;
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md_ctx || EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), nullptr) != 1)
    {
        file_stream.close();
        co_await lfs_error_response(stream, req,
            http::status::internal_server_error,
            R"({"message":"Internal Server Error"})");
        co_return;
    }

    std::string computed_oid = co_await stream_upload_to_file(
        stream, buffer, parser, connection_id,
        file_path, file_stream, md_ctx.get());

    if (computed_oid.empty())
    {
        // Error already sent by stream_upload_to_file.
        co_return;
    }

    file_stream.close();

    if (computed_oid != oid)
    {
        boost::system::error_code remove_ec;
        fs::remove(file_path, remove_ec);
        if (remove_ec)
            XLOG_ERR << "LFS Session: " << connection_id
                << ", failed to remove mismatched file: " << remove_ec.message();

        XLOG_ERR << "LFS Session: " << connection_id
            << ", SHA-256 mismatch: expected " << oid
            << ", computed " << computed_oid;

        co_await lfs_error_response(stream, req,
            http::status::bad_request,
            R"({"message":"SHA256 mismatch: OID does not match content"})");
        co_return;
    }

    http::response<http::string_body> res{
        http::status::ok, req.version()
    };
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/vnd.git-lfs+json");
    res.keep_alive(req.keep_alive());
    res.body() = R"({})";
    res.prepare_payload();
    co_await http::async_write(stream, res, ioc_awaitable[ec]);

    if (ec)
        XLOG_ERR << "LFS Session: " << connection_id
            << ", upload write response error: " << ec.message();

    co_return;
}

//////////////////////////////////////////////////////////////////////////
// LFS 文件下载处理 (GET /files/{oid})
//////////////////////////////////////////////////////////////////////////

// Stream a file to the client using buffer_body serialization.
template <typename Stream>
inline awaitable_void stream_file_download(
    Stream& stream,
    int64_t connection_id,
    http::response<http::buffer_body>& res,
    std::ifstream& file_stream)
{
    boost::system::error_code ec;

    http::response_serializer<http::buffer_body, http::fields> sr(res);

    res.body().data = nullptr;
    res.body().more = false;

    co_await http::async_write_header(stream, sr, ioc_awaitable[ec]);
    if (ec)
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", download write header error: " << ec.message();
        co_return;
    }

    constexpr std::size_t buffer_size = 5 * 1024 * 1024;
    std::vector<char> buffer(buffer_size);

    do
    {
        file_stream.read(buffer.data(), buffer_size);
        auto bytes = file_stream.gcount();

        if (bytes == 0)
        {
            res.body().data = nullptr;
            res.body().more = false;
        }
        else
        {
            res.body().data = buffer.data();
            res.body().size = bytes;
            res.body().more = true;
        }

        co_await http::async_write(stream, sr, ioc_awaitable[ec]);

        if (ec == http::error::need_buffer)
        {
            ec = {};
            continue;
        }
        if (ec)
        {
            XLOG_ERR << "LFS Session: " << connection_id
                << ", download write body error: " << ec.message();
            co_return;
        }
    } while (!sr.is_done());

    co_return;
}

template <typename Stream>
inline awaitable_void lfs_file_download_session(
    Stream& stream,
    http::request<http::dynamic_body>& req,
    int64_t connection_id,
    std::string_view oid)
{
    XLOG_DBG << "LFS Session: " << connection_id
        << ", downloading file, oid: " << oid;

    if (!lfs_detail::is_valid_oid(oid))
    {
        co_await lfs_error_response(stream, req,
            http::status::bad_request, R"({"message":"Invalid OID"})");
        co_return;
    }

    fs::path file_path = global_lfs_storage_dir / std::string(oid);

    boost::system::error_code stat_ec;
    auto file_size = fs::file_size(file_path, stat_ec);

    if (stat_ec || !fs::exists(file_path, stat_ec))
    {
        co_await lfs_error_response(stream, req,
            http::status::not_found, R"({"message":"File Not Found"})");
        co_return;
    }

    std::ifstream file_stream(
        file_path.string(),
        std::ios_base::binary | std::ios_base::in);

    if (!file_stream.is_open())
    {
        co_await lfs_error_response(stream, req,
            http::status::internal_server_error,
            R"({"message":"Internal Server Error"})");
        co_return;
    }

    http::response<http::buffer_body> res{
        http::status::ok, req.version()
    };
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/octet-stream");
    res.keep_alive(req.keep_alive());
    res.content_length(file_size);

    co_await stream_file_download(stream, connection_id, res, file_stream);

    co_return;
}

#endif // INCLUDE__2024_01_15__LFS_HPP
