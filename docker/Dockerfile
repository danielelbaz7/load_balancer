# Linux dev image for the load balancer (epoll is Linux-only).
# Alpine: ~8MB base, musl libc instead of glibc, apk package manager.
FROM alpine:3.20

RUN apk add --no-cache \
        build-base linux-headers cmake samurai \
        gdb strace \
        wrk curl netcat-openbsd iproute2 procps bash

WORKDIR /src
CMD ["bash"]
