#ifndef LOAD_BALANCER_LOAD_BALANCER_H
#define LOAD_BALANCER_LOAD_BALANCER_H

#include "backend_pool.h"
#include "listener.h"

#include <cstdint>

// Top-level object: owns the epoll instance and the Listener, and runs the event loop.
class LoadBalancer {
public:
    // Creates the epoll instance, starts the Listener on `port`,
    // and registers the listening socket with epoll.
    explicit LoadBalancer(uint16_t port);
    ~LoadBalancer();

    // Owns an fd (epfd_), so no copying.
    LoadBalancer(const LoadBalancer&) = delete;
    LoadBalancer& operator=(const LoadBalancer&) = delete;

    // The epoll_wait loop. Never returns.
    void run();

private:
    // Called by the Listener for every accepted client.
    void on_new_client(int client_fd);

    // Declaration order = construction order: epfd_ is created before listener_.
    int epfd_ = -1;
    BackendPool pool_; // declared before anything that points into it, so it's destroyed last
    Listener listener_;
};

#endif // LOAD_BALANCER_LOAD_BALANCER_H
