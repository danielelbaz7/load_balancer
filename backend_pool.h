#ifndef LOAD_BALANCER_BACKEND_POOL_H
#define LOAD_BALANCER_BACKEND_POOL_H
#include <vector>
#include <cstdint>
#include <netinet/in.h>

struct Backend {
    sockaddr_in addr;
    uint32_t active_connections = 0;
};

class BackendPool {
public:
    void add_backend(const sockaddr_in& addr);
    Backend* choose_backend();
private:
    std::vector<Backend> backends_;
};

#endif // LOAD_BALANCER_BACKEND_POOL_H
