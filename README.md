# httpd [![actions workflow](https://github.com/avplayer/httpd/actions/workflows/Build.yml/badge.svg)](https://github.com/avplayer/httpd/actions)

#### 一个基于 c++20 实现的极高性能 http 服务，可用于静态文件或标准输入设备的 http 服务器。

<br>
<br>

### Linux 平台下编译：

<br>

首先执行git克隆源码：

```
git clone <source url>
```
然后进入源码目录，执行如下操作：

```
cmake -S . -B build
cmake --build build
```
编译完成后即可得到 httpd 在 build/bin 目录下

或者使用 Docker 编译：

```
docker build -f Dockerfile.ubuntu . -t httpd:v1
```

<br>
<br>

httpd 主要用于类似以下几种场景：

```
$ tail -f x.log | httpd -l 0.0.0.0:8080   # 命令输出内容通过管道输出为httpd访问.

$ journalctl -f | httpd -l 0.0.0.0:8080   # 命令执行输出到管道通过httpd访问.

$ ffmpeg -re -i input.mp4 -c copy -f mpegts - | httpd -l 0.0.0.0:8080   # 将输入的 input.mp4 发布为 http mpegts 流.

$ httpd -l 0.0.0.0:8080 -f test.mp4   # 指定单个文件用于http访问.

$ httpd -l 0.0.0.0:8080 -f .   # 指定目录.为http doc目录, 可使用http访问目录下指定的文件.
```

比如复杂的场景，如使用 RTL-SDR 将 FM广播 转为 http + ts 流（需要RTL2832U设备）

```
$ rtl_fm -f 93.0M -M wbfm -s 200000 -r 44100 - | sox -t raw -b 16 -es -c1 -v 1 -r 44100 - -t raw - sinc 300-3000 gain 9| ffmpeg -f s16le -ac 1 -i pipe:0 -ab 128k -f mpegts - | httpd -l 0.0.0.0:8080 -f -
```

使用 curl 即可访问，如：

```
curl http://127.0.01:8080/
```

FM 可用 vlc 播放 http ts 流：

```
vlc http://127.0.01:8080/
```

或者使用浏览器查看：

![image](https://user-images.githubusercontent.com/378220/215514883-4c29f0e5-9799-4d0e-9a43-d1cf89779bd1.png)


<br>

有任何问题可加tg账号: https://t.me/jackarain 或tg群组: https://t.me/joinchat/C3WytT4RMvJ4lqxiJiIVhg
