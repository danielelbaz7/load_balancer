#ifndef LOAD_BALANCER_LISTENER_H
#define LOAD_BALANCER_LISTENER_H

#include <cstdint>
#include <functional>

// Owns the listening socket. When epoll reports it readable, accepts every
// pending client and hands each new (non-blocking) client fd to the callback.
class Listener {
public:
    using AcceptCallback = std::function<void(int client_fd)>;

    // Creates a non-blocking TCP socket bound to 0.0.0.0:port and starts listening.
    // Throws std::system_error if any setup step fails.
    Listener(uint16_t port, AcceptCallback on_accept);
    ~Listener();

    // One owner per socket: no copying (would double-close the fd).
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // The fd to register with epoll (EPOLLIN).
    int fd() const { return fd_; }

    // Called by the event loop when fd() is ready. Accepts until EAGAIN.
    void on_event(uint32_t events);

private:
    int fd_ = -1;
    AcceptCallback on_accept_;
};

#endif // LOAD_BALANCER_LISTENER_H
