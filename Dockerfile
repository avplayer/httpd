FROM alpine:edge as builder

RUN apk add -u alpine-keys --allow-untrusted
RUN apk add --no-cache fortify-headers bsd-compat-headers libgphobos libgomp libatomic binutils bash build-base make gcc musl-dev cmake ninja g++ linux-headers git bison elfutils-dev libcap-dev flex iptables-dev

ADD . /httpd

RUN cd /httpd && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS="-static" -G Ninja && ninja

FROM alpine:latest

RUN apk add --no-cache ca-certificates bash

COPY --from=builder /httpd/build/bin/httpd /usr/local/bin/

WORKDIR /root
ENTRYPOINT ["httpd"]

