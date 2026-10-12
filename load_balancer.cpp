#include "load_balancer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
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

    auto* conn = new Connection{client_fd, backend_fd, backend, false};
    conns_[client_fd] = conn;
    conns_[backend_fd] = conn;

    // Only watch the backend for now: EPOLLOUT fires when the handshake finishes.
    // The client isn't registered until then; the kernel buffers anything it sends.
    epoll_event ev{};
    ev.events = EPOLLOUT;
    ev.data.fd = backend_fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, backend_fd, &ev) < 0) {
        perror("epoll_ctl(backend)");
        close_connection(conn);
    }
}

//event with a connection that we have
void LoadBalancer::on_connection_event(int fd, uint32_t events) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return; // its Connection was already closed earlier in this epoll_wait batch
    }
    Connection* conn = it->second;

    if (fd == conn->backend_fd && !conn->connected) {
        on_backend_connected(conn);
        return;
    }

    // Either side hung up or errored.
    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        close_connection(conn);
        return;
    }

    // TODO (next step): EPOLLIN -> read from this fd and write to the other one.
}

void LoadBalancer::on_backend_connected(Connection* conn) {
    // A failed connect also shows up as "writable", so ask the socket how it went.
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(conn->backend_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;
    }
    if (err != 0) {
        std::cerr << "backend port " << ntohs(conn->backend->addr.sin_port)
                  << " connect failed: " << std::strerror(err) << std::endl;
        close_connection(conn);
        return;
    }
    conn->connected = true;

    // Watch both sides for hangups. The next step adds EPOLLIN here to forward data.
    epoll_event ev{};
    ev.events = EPOLLRDHUP;
    ev.data.fd = conn->backend_fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, conn->backend_fd, &ev) < 0) {
        perror("epoll_ctl(mod backend)");
        close_connection(conn);
        return;
    }
    ev.data.fd = conn->client_fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, conn->client_fd, &ev) < 0) {
        perror("epoll_ctl(add client)");
        close_connection(conn);
        return;
    }

    std::cout << "connected: client fd " << conn->client_fd << " <-> backend fd "
              << conn->backend_fd << std::endl;
}

void LoadBalancer::close_connection(Connection* conn) {
    // Forget both fds first: once closed, the numbers can be reused by new sockets.
    conns_.erase(conn->client_fd);
    conns_.erase(conn->backend_fd);
    close(conn->client_fd);  // close() also removes the fd from epoll
    close(conn->backend_fd);
    conn->backend->active_connections--;

    std::cout << "closed: client fd " << conn->client_fd << " <-> backend fd "
              << conn->backend_fd << std::endl;
    delete conn;
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
            } else {
                on_connection_event(events[i].data.fd, events[i].events);
            }
        }
    }
}

LoadBalancer::~LoadBalancer() {
    // Free any Connections still open. Each one is in the map twice, so only
    // handle it via its client_fd entry.
    for (auto& [fd, conn] : conns_) {
        if (fd == conn->client_fd) {
            close(conn->client_fd);
            close(conn->backend_fd);
            delete conn;
        }
    }
    conns_.clear();

    if (epfd_ < 0) {
        return;
    }
    close(epfd_);
}


