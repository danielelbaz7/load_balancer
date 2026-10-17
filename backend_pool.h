#ifndef LOAD_BALANCER_BACKEND_POOL_H
#define LOAD_BALANCER_BACKEND_POOL_H
#include <vector>
#include <cstdint>
#include <netinet/in.h>

struct Backend {
    sockaddr_in addr;
    uint32_t active_connections = 0;

    // Health state, updated by health-check probes and failed client connects.
    bool healthy = true;
    int consecutive_failures = 0;
    int consecutive_successes = 0;
    int probe_fd = -1; // in-flight health probe socket, or -1
};

class BackendPool {
public:
    void add_backend(const sockaddr_in& addr);
    // Least connections among healthy backends; nullptr if none are healthy.
    Backend* choose_backend();
    // Records a probe/connect result and flips `healthy` after enough in a row.
    void record_result(Backend& backend, bool ok);
    std::vector<Backend>& backends() { return backends_; }
private:
    std::vector<Backend> backends_;
};

#endif // LOAD_BALANCER_BACKEND_POOL_H
