#ifndef LOAD_BALANCER_CONNECTION_H
#define LOAD_BALANCER_CONNECTION_H

#include "backend_pool.h"

#include <cstdint>
#include <string>

// One proxied session: a client's socket paired with the LB's socket to its backend.
struct Connection {
    int client_fd = -1;          // LB <-> client
    int backend_fd = -1;         // LB <-> backend
    Backend* backend = nullptr;  // which server (for active_connections-- on close)
    bool connected = false;      // has the non-blocking connect to the backend finished?
    bool client_done = false;    // client stopped sending (read() returned 0)
    bool backend_done = false;   // backend stopped sending (read() returned 0)

    // Bytes read from one side that the other side's socket couldn't accept yet.
    std::string to_backend;      // from client, waiting to be sent to backend
    std::string to_client;       // from backend, waiting to be sent to client

    // The epoll masks currently registered, so we only call epoll_ctl on changes.
    uint32_t client_events = 0;
    uint32_t backend_events = 0;
};

#endif // LOAD_BALANCER_CONNECTION_H
