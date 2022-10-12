# httpd
可用于单文件或标准输入设备的 http 服务器.

<br>

### Linux 平台下编译:

<br>

首先执行git克隆源码：

```
git clone <source url>
```
然后进入源码目录，执行如下操作：

```
mkdir build && cd build
```
```
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

上面命令中，CMAKE_BUILD_TYPE=Debug指定了编译为Debug的类型，如果需要更好的性能，则需要编译为Release。

在cmake命令成功执行完成后，开始输入以下命令编译：

```
make
```
编译完成后即可得到httpd在build/bin目录下

<br>
<br>

httpd主要用于类似以下几种场景:

```
$ tail -f x.log | httpd -l 0.0.0.0:8080   # 命令输出内容通过管道输出为httpd访问.

$ journalctl -f | httpd -l 0.0.0.0:8080   # 命令执行输出到管道通过httpd访问.

$ httpd -l 0.0.0.0:8080 -f test.mp4   # 指定单个文件用于http访问.

$ httpd -l 0.0.0.0:8080 -f .   # 指定目录.为http doc目录, 可使用http访问目录下指定的文件.
```

比如复杂的场景，如使用 RTL-SDR 将 FM广播 转为 http + ts 流（需要RTL2832U设备）

```
$ rtl_fm -f 93.0M -M wbfm -s 200000 -r 44100 - | sox -t raw -b 16 -es -c1 -v 1 -r 44100 - -t raw - sinc 300-3000 gain 9| ffmpeg -f s16le -ac 1 -i pipe:0 -ab 128k -f mpegts - | httpd -l 0.0.0.0:9300 -f -
```

使用curl即可访问，如：

```
curl http://127.0.01:8080/
```

FM可用vlc播放http ts流：

```
vlc http://127.0.01:9300/
```
<br>

Happy hacking!
