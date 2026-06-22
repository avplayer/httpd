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

        // 读取请求体并写入文件.
        {
            std::string upload_data = beast::buffers_to_string(req.body().data());
            file_stream.write(upload_data.data(), upload_data.size());
        }

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
