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
// LFS 批处理请求处理 (POST /objects/batch)
//////////////////////////////////////////////////////////////////////////

template <typename Stream>
inline awaitable_void lfs_batch_session(
    Stream& stream,
    http::request<http::dynamic_body>& req,
    int64_t connection_id,
    bool is_ssl = false)
{
    boost::system::error_code ec;

    XLOG_DBG << "LFS Session: " << connection_id << ", batch request";

    // 读取请求体并解析 JSON.
    std::string body_str = beast::buffers_to_string(req.body().data());
    json::value req_json;
    bool parse_ok = false;
    try
    {
        req_json = json::parse(body_str);
        parse_ok = true;
    }
    catch (const std::exception& e)
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", invalid JSON: " << e.what();
    }

    if (!parse_ok)
    {
        http::response<http::string_body> res{
            http::status::bad_request,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Invalid JSON"})";
        res.prepare_payload();

        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    // 提取操作类型.
    std::string operation;
    if (req_json.kind() == json::kind::object)
    {
        auto op_it = req_json.get_object().find("operation");
        if (op_it != req_json.get_object().end() &&
            op_it->value().kind() == json::kind::string)
        {
            operation = op_it->value().get_string().c_str();
        }
    }

    if (operation != "upload" && operation != "download")
    {
        http::response<http::string_body> res{
            http::status::bad_request,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Invalid operation"})";
        res.prepare_payload();

        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    // 构建响应.
    // 根据当前连接是否使用 SSL 来决定协议前缀.
    std::string server_url = is_ssl ? "https://" : "http://";
    auto host_it = req.find(http::field::host);
    if (host_it != req.end())
        server_url += host_it->value();
    else
        server_url += "localhost:8080";

    auto resp_json = lfs_detail::build_batch_response(
        req_json, operation, global_lfs_storage_dir, server_url);

    std::string resp_body = json::serialize(resp_json);

    http::response<http::string_body> res{
        http::status::ok,
        req.version()
    };
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/vnd.git-lfs+json");
    res.keep_alive(req.keep_alive());
    res.content_length(resp_body.size());
    res.body() = std::move(resp_body);
    res.prepare_payload();

    co_await http::async_write(stream, res, ioc_awaitable[ec]);

    if (ec)
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", batch write error: " << ec.message();
    }

    co_return;
}

//////////////////////////////////////////////////////////////////////////
// LFS 文件上传处理 (PUT /files/{oid}) — 流式读取 body，避免内存暴涨
//////////////////////////////////////////////////////////////////////////

template <typename Stream>
inline awaitable_void lfs_file_upload_session(
    Stream& stream,
    beast::flat_buffer& buffer,
    http::request<http::dynamic_body>& req,
    int64_t connection_id,
    std::string_view oid)
{
    boost::system::error_code ec;

    XLOG_DBG << "LFS Session: " << connection_id
        << ", file upload (streaming), oid: " << oid;

    // 校验 OID.
    if (!lfs_detail::is_valid_oid(oid))
    {
        http::response<http::string_body> res{
            http::status::bad_request,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Invalid OID"})";
        res.prepare_payload();
        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    fs::path file_path = global_lfs_storage_dir / std::string(oid);

    XLOG_DBG << "LFS Session: " << connection_id
        << ", uploading file: " << file_path;

    // 确保存储目录存在.
    boost::system::error_code mkdir_ec;
    fs::create_directories(global_lfs_storage_dir, mkdir_ec);

    std::ofstream file_stream(
        file_path.string(),
        std::ios_base::binary | std::ios_base::trunc);

    if (!file_stream.is_open())
    {
        http::response<http::string_body> res{
            http::status::internal_server_error,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Internal Server Error"})";
        res.prepare_payload();
        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    // 初始化 SHA-256 哈希计算上下文.
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!md_ctx || EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), nullptr) != 1)
    {
        http::response<http::string_body> res{
            http::status::internal_server_error,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Internal Server Error"})";
        res.prepare_payload();
        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    // 从请求头创建 buffer_body 解析器，流式读取 body.
    http::request<http::buffer_body> buffer_req{std::move(req).base()};
    http::request_parser<http::buffer_body> body_parser{std::move(buffer_req)};
    body_parser.body_limit(std::numeric_limits<uint64_t>::max());

    // 分块读取 body，默认 64KB 一块.
    constexpr std::size_t chunk_size = 64 * 1024;
    std::vector<char> chunk(chunk_size);

    for (;;)
    {
        auto& body = body_parser.get().body();
        body.data = chunk.data();
        body.size = chunk.size();
        body.more = true;

        co_await http::async_read(stream, buffer, body_parser, ioc_awaitable[ec]);

        std::size_t bytes_written = chunk.size() - body.size;
        if (bytes_written > 0)
        {
            file_stream.write(chunk.data(), bytes_written);
            if (!file_stream)
            {
                XLOG_ERR << "LFS Session: " << connection_id
                    << ", file write failed";

                file_stream.close();
                boost::system::error_code remove_ec;
                fs::remove(file_path, remove_ec);

                http::response<http::string_body> res{
                    http::status::internal_server_error,
                    body_parser.get().version()
                };
                res.set(http::field::server, "httpd/1.0");
                res.set(http::field::content_type, "application/vnd.git-lfs+json");
                res.keep_alive(body_parser.get().keep_alive());
                res.body() = R"({"message":"Upload Failed"})";
                res.prepare_payload();
                co_await http::async_write(stream, res, ioc_awaitable[ec]);
                co_return;
            }

            EVP_DigestUpdate(md_ctx.get(), chunk.data(), bytes_written);
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

            http::response<http::string_body> res{
                http::status::internal_server_error,
                body_parser.get().version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(body_parser.get().keep_alive());
            res.body() = R"({"message":"Upload Failed"})";
            res.prepare_payload();
            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }
        break;
    }

    // 完成哈希计算.
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(md_ctx.get(), hash, &hash_len);

    // 将哈希值转换为十六进制字符串.
    static const char hex_chars[] = "0123456789abcdef";
    std::string computed_oid;
    computed_oid.reserve(64);
    for (unsigned int i = 0; i < hash_len; ++i)
    {
        computed_oid += hex_chars[(hash[i] >> 4) & 0x0F];
        computed_oid += hex_chars[hash[i] & 0x0F];
    }

    // 校验哈希是否与请求中的 OID 一致.
    if (computed_oid != oid)
    {
        file_stream.close();
        boost::system::error_code remove_ec;
        fs::remove(file_path, remove_ec);
        if (remove_ec)
        {
            XLOG_ERR << "LFS Session: " << connection_id
                << ", failed to remove mismatched file: " << remove_ec.message();
        }

        XLOG_ERR << "LFS Session: " << connection_id
            << ", SHA-256 mismatch: expected " << oid
            << ", computed " << computed_oid;

        http::response<http::string_body> res{
            http::status::bad_request,
            body_parser.get().version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(body_parser.get().keep_alive());
        res.body() = R"({"message":"SHA256 mismatch: OID does not match content"})";
        res.prepare_payload();
        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    file_stream.close();

    http::response<http::string_body> res{
        http::status::ok,
        body_parser.get().version()
    };
    res.set(http::field::server, "httpd/1.0");
    res.set(http::field::content_type, "application/vnd.git-lfs+json");
    res.keep_alive(body_parser.get().keep_alive());
    res.body() = R"({})";
    res.prepare_payload();
    co_await http::async_write(stream, res, ioc_awaitable[ec]);

    if (ec)
    {
        XLOG_ERR << "LFS Session: " << connection_id
            << ", upload write response error: " << ec.message();
    }

    co_return;
}

//////////////////////////////////////////////////////////////////////////
// LFS 文件传输处理 (PUT/GET /files/{oid})
//////////////////////////////////////////////////////////////////////////

template <typename Stream>
inline awaitable_void lfs_file_transfer_session(
    Stream& stream,
    http::request<http::dynamic_body>& req,
    int64_t connection_id,
    std::string_view oid)
{
    boost::system::error_code ec;

    XLOG_DBG << "LFS Session: " << connection_id
        << ", file transfer, method: "
        << std::string(req.method_string())
        << ", oid: " << oid;

    // 校验 OID.
    if (!lfs_detail::is_valid_oid(oid))
    {
        http::response<http::string_body> res{
            http::status::bad_request,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Invalid OID"})";
        res.prepare_payload();

        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        co_return;
    }

    fs::path file_path = global_lfs_storage_dir / std::string(oid);

    switch (req.method())
    {
    case http::verb::put:
    {
        // 客户端上传文件.
        XLOG_DBG << "LFS Session: " << connection_id
            << ", uploading file: " << file_path;

        // 确保存储目录存在.
        boost::system::error_code mkdir_ec;
        fs::create_directories(global_lfs_storage_dir, mkdir_ec);

        std::ofstream file_stream(
            file_path.string(),
            std::ios_base::binary | std::ios_base::trunc);

        if (!file_stream.is_open())
        {
            http::response<http::string_body> res{
                http::status::internal_server_error,
                req.version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"message":"Internal Server Error"})";
            res.prepare_payload();

            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }

        // 初始化 SHA-256 哈希计算上下文.
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md_ctx(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!md_ctx || EVP_DigestInit_ex(md_ctx.get(), EVP_sha256(), nullptr) != 1)
        {
            http::response<http::string_body> res{
                http::status::internal_server_error,
                req.version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"message":"Internal Server Error"})";
            res.prepare_payload();

            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }

        // 读取请求体并写入文件，同时计算 SHA-256 哈希.
        {
            std::string upload_data = beast::buffers_to_string(req.body().data());
            file_stream.write(upload_data.data(), upload_data.size());

            if (!file_stream)
            {
                http::response<http::string_body> res{
                    http::status::internal_server_error,
                    req.version()
                };
                res.set(http::field::server, "httpd/1.0");
                res.set(http::field::content_type, "application/vnd.git-lfs+json");
                res.keep_alive(req.keep_alive());
                res.body() = R"({"message":"Upload Failed"})";
                res.prepare_payload();

                co_await http::async_write(stream, res, ioc_awaitable[ec]);
                co_return;
            }

            EVP_DigestUpdate(md_ctx.get(), upload_data.data(), upload_data.size());
        }

        // 完成哈希计算.
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;
        EVP_DigestFinal_ex(md_ctx.get(), hash, &hash_len);

        // 将哈希值转换为十六进制字符串.
        static const char hex_chars[] = "0123456789abcdef";
        std::string computed_oid;
        computed_oid.reserve(64);
        for (unsigned int i = 0; i < hash_len; ++i)
        {
            computed_oid += hex_chars[(hash[i] >> 4) & 0x0F];
            computed_oid += hex_chars[hash[i] & 0x0F];
        }

        // 校验哈希是否与请求中的 OID 一致.
        if (computed_oid != oid)
        {
            file_stream.close();
            boost::system::error_code remove_ec;
            fs::remove(file_path, remove_ec);
            if (remove_ec)
            {
                XLOG_ERR << "LFS Session: " << connection_id
                    << ", failed to remove mismatched file: " << remove_ec.message();
            }

            XLOG_ERR << "LFS Session: " << connection_id
                << ", SHA-256 mismatch: expected " << oid
                << ", computed " << computed_oid;

            http::response<http::string_body> res{
                http::status::bad_request,
                req.version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"message":"SHA256 mismatch: OID does not match content"})";
            res.prepare_payload();

            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }

        file_stream.close();

        http::response<http::string_body> res{
            http::status::ok,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({})";
        res.prepare_payload();

        co_await http::async_write(stream, res, ioc_awaitable[ec]);

        if (ec)
        {
            XLOG_ERR << "LFS Session: " << connection_id
                << ", upload write response error: " << ec.message();
        }

        break;
    }

    case http::verb::get:
    {
        // 客户端下载文件.
        XLOG_DBG << "LFS Session: " << connection_id
            << ", downloading file: " << file_path;

        boost::system::error_code stat_ec;
        auto file_size = fs::file_size(file_path, stat_ec);

        if (stat_ec || !fs::exists(file_path, stat_ec))
        {
            http::response<http::string_body> res{
                http::status::not_found,
                req.version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"message":"File Not Found"})";
            res.prepare_payload();

            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }

        std::ifstream file_stream(
            file_path.string(),
            std::ios_base::binary | std::ios_base::in);

        if (!file_stream.is_open())
        {
            http::response<http::string_body> res{
                http::status::internal_server_error,
                req.version()
            };
            res.set(http::field::server, "httpd/1.0");
            res.set(http::field::content_type, "application/vnd.git-lfs+json");
            res.keep_alive(req.keep_alive());
            res.body() = R"({"message":"Internal Server Error"})";
            res.prepare_payload();

            co_await http::async_write(stream, res, ioc_awaitable[ec]);
            co_return;
        }

        // 使用 buffer_body 发送文件内容.
        http::response<http::buffer_body> res{
            http::status::ok,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.content_length(file_size);

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
        std::streamsize total = 0;

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

        break;
    }

    default:
    {
        http::response<http::string_body> res{
            http::status::method_not_allowed,
            req.version()
        };
        res.set(http::field::server, "httpd/1.0");
        res.set(http::field::content_type, "application/vnd.git-lfs+json");
        res.keep_alive(req.keep_alive());
        res.body() = R"({"message":"Method Not Allowed"})";
        res.prepare_payload();

        co_await http::async_write(stream, res, ioc_awaitable[ec]);
        break;
    }
    }

    co_return;
}

#endif // INCLUDE__2024_01_15__LFS_HPP
