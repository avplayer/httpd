# httpd

[![Build Status](https://github.com/avplayer/httpd/actions/workflows/Build.yml/badge.svg)](https://github.com/avplayer/httpd/actions)

**httpd** 是一个基于 **C++20** 协程实现的高性能 HTTP 服务器，支持静态文件服务、目录列表以及标准输入管道流式输出。

---

## 特性

- **高性能** — 基于 Boost.Beast 和 Boost.Asio 异步 I/O，使用 C++20 协程（`co_await`）实现全异步非阻塞架构
- **管道模式** — 从标准输入读取数据，实时推送给 HTTP 客户端（适用于日志 tail、音视频流等场景）
- **静态文件服务** — 支持单个文件或整个目录的 HTTP/HTTPS 访问
- **目录列表** — 自动生成 HTML 目录索引，支持 `?q=json` 查询参数返回 JSON 格式目录列表
- **断点续传** — 支持 HTTP Range 请求（partial content），可用于视频拖动播放
- **HTTPS 支持** — 通过 `--ssl-cert-dir` 指定证书目录自动加载证书
- **自动证书发现** — 自动扫描证书目录，优先使用完整链证书（`fullchain.pem`），并从证书中提取域名
- **MIME 类型** — 内置常用文件扩展名的 MIME 映射
- **IPv4/IPv6 双栈** — 默认监听 `[::0]:80`，同时支持 IPv4 和 IPv6
- **Keep-Alive** — 支持 HTTP/1.1 持久连接
- **Git LFS 支持** — 通过 `--lfs-storage-dir` 启用 Git LFS Batch API 和文件传输端点（基于 Git LFS 协议 v1 批量 API）
- **索引文件** — 目录访问时自动寻找并返回 `index.html` / `index.htm`
- **100-continue** — 支持 `Expect: 100-continue` 请求头
- **跨平台** — 支持 Linux、macOS、Windows
- **io_uring** — Linux 平台可选启用 `io_uring` 支持（`ENABLE_USE_IO_URING`）
- **Docker** — 提供多阶段构建 Dockerfile（Alpine 静态链接 / Ubuntu）
- **systemd** — 提供 systemd 服务文件，以及支持输出 systemd 日志

---

## 环境要求

- **编译器**: GCC 11+ / Clang 14+ / MSVC 2019 16.11+
- **构建系统**: CMake 3.16+
- **C++ 标准**: C++20

> 该项目使用 Monorepo 方案，所有第三方依赖库（Boost、fmt、OpenSSL、zlib）均已包含在 `third_party/` 目录中，无需额外安装。

---

## 编译与安装

### Linux / macOS

```bash
# 克隆源码
git clone <repository-url>
cd httpd

# 配置与编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 编译完成后，二进制文件位于 build/bin/httpd
```

### Windows (MSVC)

```bash
cmake -S . -B build
cmake --build build --config Release
```


### Docker

```bash
# 基于 Ubuntu 22.04 编译
docker build -f Dockerfile.ubuntu . -t httpd:v1

# 基于 Alpine（静态链接，体积更小）
docker build . -t httpd:v1
```

---

## 使用方法

```text
httpd [选项]
```

### 命令行选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `-h`, `--help` | | 显示帮助信息 |
| `--listen <ip:port>` | `[::0]:80` | 监听地址和端口（支持 IPv4 和 IPv6）。当指定 `--ssl-cert-dir` 时，该地址提供 HTTPS 服务 |
| `--path <path>` | 标准输入 | 文档根目录、单个文件路径；设为 `-` 或留空则启用标准输入管道模式 |
| `--ssl-cert-dir <dir>` | 未启用 | SSL 证书目录。指定后启用 HTTPS，自动扫描目录下的证书和密钥文件 |
| `--lfs-storage-dir <dir>` | 未启用 | Git LFS 存储目录。指定后启用 Git LFS Batch API 和文件传输端点 |

> 由于启用了 `allow_long_disguise`，`--listen` 也可写作 `-listen`，`--path` 也可写作 `-p`。

---

## 使用示例

### 管道输出模式

将标准输入通过 HTTP 实时输出（可用于日志监控、流媒体等）：

```bash
# 实时推送日志到 HTTP 客户端
tail -f /var/log/system.log | httpd --listen 0.0.0.0:8080 --path -

# 推送 journald 日志
journalctl -f | httpd --listen 0.0.0.0:8080 --path -

# 将视频文件发布为 HTTP MPEGTS 流
ffmpeg -re -i input.mp4 -c copy -f mpegts - | httpd --listen 0.0.0.0:8080 --path -
```

### 静态文件服务

```bash
# 提供单个文件
httpd --listen 0.0.0.0:8080 --path test.mp4

# 提供整个目录作为 HTTP 文档根目录
httpd --listen 0.0.0.0:8080 --path /var/www/html
```

### HTTPS 服务

指定 `--ssl-cert-dir` 参数启用 HTTPS，程序会自动扫描证书目录并加载证书：

```bash
# 使用 Let's Encrypt 证书（证书目录结构需包含 fullchain.pem + privkey.pem）
httpd --listen [::0]:443 --ssl-cert-dir /etc/letsencrypt/live/example.com/ --path /var/www/html
```

#### 证书目录文件命名规则

程序按以下优先级自动查找证书-密钥对：

| 优先级 | 证书文件 | 密钥文件 |
|--------|---------|---------|
| 最高 | `fullchain.pem` | `privkey.pem` |
| 高 | `cert.pem` | `key.pem` |
| 中 | `*.crt` | `*.key` |
| 低 | `*.pem` | — |

如果文件名带有共同前缀（如 `example.crt` + `example.key`），程序会自动配对加载。

启动时会输出发现的证书及其域名信息：

```
SSL certificate loaded: fullchain.pem (domain: example.com)
```

### JSON 格式目录列表

访问目录时附加 `?q=json` 参数，可获取 JSON 格式的目录结构：

```bash
curl "http://127.0.0.1:8080/path/to/dir/?q=json"
# HTTPS 示例
curl -k "https://127.0.0.1/path/to/dir/?q=json"
```

通过返回目录列表为 JSON，可用于实现在线音乐播放器，参考：[music_player](https://github.com/Jackarain/proxy/tree/master/example/music_player)，仅需要复制链接中的 `index.html` 文件到 `httpd` 参数 `--path` 指定的目录下，并将您的音乐文件一并复制到该目录下即可实现一个自建的在线音乐播放器。

### 高级应用 — FM 广播转 HTTP 流

将 RTL-SDR 接收的 FM 广播转换为 HTTP TS 流（需要 RTL2832U 设备）：

```bash
rtl_fm -f 93.0M -M wbfm -s 200000 -r 44100 - \
  | sox -t raw -b 16 -es -c1 -v 1 -r 44100 - -t raw - sinc 300-3000 gain 9 \
  | ffmpeg -f s16le -ac 1 -i pipe:0 -ab 128k -f mpegts - \
  | httpd --listen 0.0.0.0:8080 --path -
```

### Git LFS 服务

指定 `--lfs-storage-dir` 参数启用内置 Git LFS 服务器，基于 [Git LFS Batch API](https://github.com/git-lfs/git-lfs/blob/main/docs/api/batch.md) 协议：

```bash
# 启动 Git LFS 服务（存储目录为 /srv/lfs-objects）
httpd --listen 0.0.0.0:8080 --lfs-storage-dir /srv/lfs-objects --path /var/www/html
```

在 Git 仓库中配置 LFS 端点：

```bash
# 配置 LFS 端点指向 httpd 服务
git config lfs.url http://127.0.0.1:8080
# 或使用 .lfsconfig 文件
echo '[lfs]
url = http://127.0.0.1:8080' > .lfsconfig
```

支持以下 Git LFS 操作：

- **上传** — `PUT /files/<oid>` 存储 LFS 对象
- **下载** — `GET /files/<oid>` 获取 LFS 对象
- **批处理查询** — `POST /objects/batch` 批处理请求

> **注意**：LFS 服务仅支持 Basic 传输模式，存储的对象以 SHA-256 OID 为文件名保存在指定目录中。

### 访问服务

```bash
# HTTP 访问
curl http://127.0.0.1:8080/

# HTTPS 访问（使用 --ssl-cert-dir 时）
curl -k https://127.0.0.1:443/

# 使用 VLC 播放 TS 流
vlc http://127.0.0.1:8080/

# 使用浏览器查看目录列表
open http://127.0.0.1:8080/
```

浏览器访问效果：

![目录列表截图](https://user-images.githubusercontent.com/378220/215514883-4c29f0e5-9799-4d0e-9a43-d1cf89779bd1.png)

---

## systemd 服务

项目提供了 systemd 服务文件 `httpd.service`，安装后可使用 systemctl 管理：

```bash
sudo cp build/httpd.service /usr/lib/systemd/system/
sudo systemctl enable httpd
sudo systemctl start httpd
```

---

## 许可证

- **httpd** 基于 [Boost Software License 1.0](http://www.boost.org/LICENSE_1_0.txt) 发布。
- 部分第三方库可能使用其他开源许可证，详情请参考各库的 LICENSE 文件。

---

## 作者与联系

- **作者**: Jack (jack.arain at gmail dot com)
- **Telegram**: [@jackarain](https://t.me/jackarain) | [加入群组](https://t.me/joinchat/C3WytT4RMvJ4lqxiJiIVhg)
