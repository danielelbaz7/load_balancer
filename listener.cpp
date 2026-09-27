#include "listener.h"

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

Listener::Listener(uint16_t port, AcceptCallback on_accept)
    : on_accept_(std::move(on_accept)) {
    // Non-blocking from birth; CLOEXEC so the fd isn't leaked into child processes.
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    // If a constructor throws, the destructor never runs, so close the fd ourselves.
    auto fail = [this](const char* what) {
        int err = errno; // save it: close() could overwrite errno
        close(fd_);
        fd_ = -1;
        throw std::system_error(err, std::generic_category(), what);
    };

    int one = 1;
    // Allow rebinding right after a restart while old connections sit in TIME_WAIT.
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        fail("setsockopt(SO_REUSEADDR)");
    }
    // Let several listeners (one per worker thread, later) bind the same port;
    // the kernel load-balances incoming connections across them.
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        fail("setsockopt(SO_REUSEPORT)");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);              // host -> network (big-endian) byte order
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0: needed for Docker port forwarding

    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fail("bind");
    }
    if (listen(fd_, SOMAXCONN) < 0) {
        fail("listen");
    }
}

void Listener::on_event(uint32_t events) {
    while (true) {
        int client_fd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("accept4");
            break;
        }
        if (client_fd >= 0) {
            on_accept_(client_fd);
        }
    }
}

Listener::~Listener() {
    close(fd_);
}


