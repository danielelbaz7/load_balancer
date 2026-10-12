#ifndef LOAD_BALANCER_CONNECTION_H
#define LOAD_BALANCER_CONNECTION_H

#include "backend_pool.h"

// One proxied session: a client's socket paired with the LB's socket to its backend.
struct Connection {
    int client_fd = -1;          // LB <-> client
    int backend_fd = -1;         // LB <-> backend
    Backend* backend = nullptr;  // which server (for active_connections-- on close)
    bool connected = false;      // has the non-blocking connect to the backend finished?
};

#endif // LOAD_BALANCER_CONNECTION_H
