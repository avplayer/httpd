# httpd
用于单文件，管道的简单http服务器.

用于类似以下场景:

```
$ tail -f x.log|./httpd 8080

$ find /|./httpd 8080

$ ./httpd 8080 test.mp4

```
然后使用
```
curl http://127.0.01:8080/
```
即可下载获取相关的信息输出.
