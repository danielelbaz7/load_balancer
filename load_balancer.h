#ifndef LOAD_BALANCER_LOAD_BALANCER_H
#define LOAD_BALANCER_LOAD_BALANCER_H

#include "backend_pool.h"
#include "connection.h"
#include "listener.h"

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <vector>

// Top-level object: owns the epoll instance and the Listener, and runs the event loop.
class LoadBalancer {
public:
    // Creates the epoll instance, starts the Listener on `port`, registers the
    // listening socket with epoll, and fills the pool. The backend list is fixed
    // after this: the pool never grows, so Backend* pointers stay valid.
    LoadBalancer(uint16_t port, const std::vector<sockaddr_in>& backends);
    ~LoadBalancer();

    // Owns an fd (epfd_), so no copying.
    LoadBalancer(const LoadBalancer&) = delete;
    LoadBalancer& operator=(const LoadBalancer&) = delete;

    // The epoll_wait loop. Never returns.
    void run();

private:
    // Called by the Listener for every accepted client.
    void on_new_client(int client_fd);

    // Called from run() for any fd that belongs to a Connection.
    void on_connection_event(int fd, uint32_t events);

    // The backend's non-blocking connect finished (successfully or not).
    void on_backend_connected(Connection* conn);

    // Closes both sockets, forgets both fds, and frees the Connection.
    void close_connection(Connection* conn);

    // Reads from one side until EAGAIN and sends it to the other side.
    // Returns false if the connection should be closed.
    bool forward(Connection* conn, bool from_client);

    // Sends bytes that were waiting for `fd`. Returns false on error.
    bool flush(int fd, std::string& waiting);

    // Updates both fds' epoll masks to match what's waiting (backpressure).
    bool update_interest(Connection* conn);

    // Declaration order = construction order: epfd_ is created before listener_.
    int epfd_ = -1;
    BackendPool pool_; // declared before anything that points into it, so it's destroyed last
    // Both fds of a Connection map to the same Connection. Owns the Connections
    // (new in on_new_client, delete in close_connection).
    std::unordered_map<int, Connection*> conns_;
    Listener listener_;
};

#endif // LOAD_BALANCER_LOAD_BALANCER_H
