#include "load_balancer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>

// Seconds between health-check probes.
static constexpr int kHealthIntervalSec = 2;

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

    // Health-check timer: a timerfd becomes readable every interval, so epoll can
    // wake us for it like any socket.
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        int err = errno;
        close(epfd_);
        throw std::system_error(err, std::generic_category(), "timerfd_create");
    }
    itimerspec interval{};
    interval.it_value.tv_sec = kHealthIntervalSec;    // first tick
    interval.it_interval.tv_sec = kHealthIntervalSec; // then every interval
    epoll_event tev{};
    tev.events = EPOLLIN;
    tev.data.fd = timer_fd_;
    if (timerfd_settime(timer_fd_, 0, &interval, nullptr) < 0 ||
        epoll_ctl(epfd_, EPOLL_CTL_ADD, timer_fd_, &tev) < 0) {
        int err = errno;
        close(timer_fd_);
        close(epfd_);
        throw std::system_error(err, std::generic_category(), "health timer setup");
    }
}

void LoadBalancer::on_health_tick() {
    uint64_t expirations;
    while (read(timer_fd_, &expirations, sizeof(expirations)) > 0) {
        // drain the timer so epoll stops reporting it until the next tick
    }

    for (Backend& backend : pool_.backends()) {
        // A probe from the last tick that still hasn't connected counts as a failure
        // (this is the probe's timeout).
        if (backend.probe_fd >= 0) {
            probes_.erase(backend.probe_fd);
            close(backend.probe_fd);
            backend.probe_fd = -1;
            pool_.record_result(backend, false);
        }

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            perror("socket(probe)");
            continue;
        }
        if (connect(fd, reinterpret_cast<const sockaddr*>(&backend.addr), sizeof(backend.addr)) < 0
            && errno != EINPROGRESS) {
            close(fd); // refused right away
            pool_.record_result(backend, false);
            continue;
        }
        epoll_event ev{};
        ev.events = EPOLLOUT; // writable = connect finished
        ev.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl(probe)");
            close(fd);
            continue;
        }
        backend.probe_fd = fd;
        probes_[fd] = &backend;
    }
}

void LoadBalancer::on_probe_done(int probe_fd) {
    auto it = probes_.find(probe_fd);
    if (it == probes_.end()) {
        return;
    }
    Backend* backend = it->second;
    probes_.erase(it);

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(probe_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;
    }
    close(probe_fd);
    backend->probe_fd = -1;
    pool_.record_result(*backend, err == 0);
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

    // Socket error or full hangup: nothing more can be sent or received.
    if (events & (EPOLLHUP | EPOLLERR)) {
        close_connection(conn);
        return;
    }

    bool is_client = (fd == conn->client_fd);

    // This fd has room again: send what was waiting for it.
    if (events & EPOLLOUT) {
        std::string& waiting = is_client ? conn->to_client : conn->to_backend;
        if (!flush(fd, waiting)) {
            close_connection(conn);
            return;
        }
    }

    // Data arrived, or the peer stopped sending (RDHUP). Either way, read: forward()
    // passes along any last bytes and marks the side done when read() returns 0.
    if (events & (EPOLLIN | EPOLLRDHUP)) {
        if (!forward(conn, is_client)) {
            close_connection(conn);
            return;
        }
    }

    // A side that stopped sending is finished only once everything it sent has been
    // delivered. E.g. the backend sends a response and closes, but a slow client
    // hasn't received all of it yet: keep the connection until to_client is empty.
    if ((conn->backend_done && conn->to_client.empty()) ||
        (conn->client_done && conn->to_backend.empty())) {
        close_connection(conn);
        return;
    }

    if (!update_interest(conn)) {
        close_connection(conn);
    }
}

// Sends as much of [data, data+len) as the socket accepts right now.
// Returns bytes sent (may be less than len if the socket is full), or -1 on error.
// MSG_NOSIGNAL: writing to a closed socket returns EPIPE instead of killing us with SIGPIPE.
static ssize_t send_some(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (w > 0) {
            sent += static_cast<size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // socket's send buffer is full; the rest waits
        } else {
            return -1;
        }
    }
    return static_cast<ssize_t>(sent);
}

bool LoadBalancer::forward(Connection* conn, bool from_client) {
    int from = from_client ? conn->client_fd : conn->backend_fd;
    int to = from_client ? conn->backend_fd : conn->client_fd;
    std::string& waiting = from_client ? conn->to_backend : conn->to_client;

    char buf[16384];
    // Stop reading once the other side is backed up (backpressure): the kernel
    // keeps the rest in this socket's receive buffer until we resume.
    while (waiting.empty()) {
        ssize_t n = read(from, buf, sizeof(buf));
        if (n > 0) {
            ssize_t sent = send_some(to, buf, static_cast<size_t>(n));
            if (sent < 0) {
                return false;
            }
            if (sent < n) {
                waiting.append(buf + sent, static_cast<size_t>(n - sent));
            }
        } else if (n == 0) {
            // Peer stopped sending. Don't close yet: bytes may still be waiting
            // to go the other way. on_connection_event decides when to close.
            (from_client ? conn->client_done : conn->backend_done) = true;
            return true;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // read everything available for now
        } else {
            return false;
        }
    }
    return true;
}

bool LoadBalancer::flush(int fd, std::string& waiting) {
    ssize_t sent = send_some(fd, waiting.data(), waiting.size());
    if (sent < 0) {
        return false;
    }
    waiting.erase(0, static_cast<size_t>(sent));
    return true;
}

bool LoadBalancer::update_interest(Connection* conn) {
    // Read from a side only while it's still sending and nothing is waiting to go
    // to the other side (backpressure). RDHUP is only watched together with EPOLLIN:
    // otherwise a paused or finished side would report "hung up" over and over.
    // Ask for EPOLLOUT on a side only while something is waiting to go to it.
    bool read_client = !conn->client_done && conn->to_backend.empty();
    bool read_backend = !conn->backend_done && conn->to_client.empty();
    uint32_t client = (read_client ? EPOLLIN | EPOLLRDHUP : 0)
                    | (conn->to_client.empty() ? 0 : EPOLLOUT);
    uint32_t backend = (read_backend ? EPOLLIN | EPOLLRDHUP : 0)
                     | (conn->to_backend.empty() ? 0 : EPOLLOUT);

    // Only call epoll_ctl when the mask actually changes (it's a system call).
    epoll_event ev{};
    if (client != conn->client_events) {
        ev.events = client;
        ev.data.fd = conn->client_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, conn->client_fd, &ev) < 0) {
            return false;
        }
        conn->client_events = client;
    }
    if (backend != conn->backend_events) {
        ev.events = backend;
        ev.data.fd = conn->backend_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, conn->backend_fd, &ev) < 0) {
            return false;
        }
        conn->backend_events = backend;
    }
    return true;
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
        pool_.record_result(*conn->backend, false); // passive health check
        close_connection(conn);
        return;
    }
    conn->connected = true;

    // Watch both sides for data and hangups.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    conn->client_events = ev.events;
    conn->backend_events = ev.events;
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
            int fd = events[i].data.fd;
            if (fd == listener_.fd()) {
                listener_.on_event(events[i].events);
            } else if (fd == timer_fd_) {
                on_health_tick();
            } else if (probes_.count(fd)) {
                on_probe_done(fd);
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

    for (auto& [fd, backend] : probes_) {
        close(fd);
    }
    if (timer_fd_ >= 0) {
        close(timer_fd_);
    }

    if (epfd_ < 0) {
        return;
    }
    close(epfd_);
}


