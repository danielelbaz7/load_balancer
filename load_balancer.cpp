#include "load_balancer.h"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

LoadBalancer::LoadBalancer(uint16_t port, const std::vector<sockaddr_in>& backends)
    : epfd_(epoll_create1(EPOLL_CLOEXEC)),
      listener_(port, [this](int fd) { on_new_client(fd); }) {

    if (epfd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    // Tell the listener to wake up when the listening socket's accept queue has clients.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listener_.fd();
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listener_.fd(), &ev) < 0) {
        int err = errno; // save before close() can overwrite it
        close(epfd_);    // destructor won't run if we throw, so clean up here
        throw std::system_error(err, std::generic_category(), "epoll_ctl");
    }

    for (const sockaddr_in& addr : backends) {
        pool_.add_backend(addr);
    }
}

void LoadBalancer::on_new_client(int client_fd) {
    Backend* backend = pool_.choose_backend();
    if (backend == nullptr) { // no backends to send this client to
        close(client_fd);
        return;
    }

    int backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (backend_fd < 0) {
        perror("socket");
        close(client_fd);
        return;
    }

    // Non-blocking connect: EINPROGRESS means the handshake has started and will finish
    // later; epoll reports backend_fd as writable (EPOLLOUT) when it does.
    if (connect(backend_fd, reinterpret_cast<const sockaddr*>(&backend->addr), sizeof(backend->addr)) < 0
        && errno != EINPROGRESS) {
        perror("connect");
        close(backend_fd);
        close(client_fd);
        return;
    }

    backend->active_connections++;
    std::cout << "client fd " << client_fd << " -> backend port " << ntohs(backend->addr.sin_port)
              << " (backend fd " << backend_fd << ")" << std::endl;

    // TODO: create a Connection, map both fds to it, and register them with epoll.
    // Until then nothing tracks these sockets, so close them now to avoid leaking fds.
    close(backend_fd);
    close(client_fd);
    backend->active_connections--;
}

void LoadBalancer::run() {
    epoll_event events[64];
    while (true) {
        // Sleep until at least one registered fd is ready (-1 = no timeout).
        int n = epoll_wait(epfd_, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // interrupted by a signal, not a real error
            }
            perror("epoll_wait");
            continue;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listener_.fd()) {
                listener_.on_event(events[i].events);
            }
        }
    }
}

LoadBalancer::~LoadBalancer() {
    if (epfd_ < 0) {
        return;
    }
    close(epfd_);
}


