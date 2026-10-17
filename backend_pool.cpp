#include "backend_pool.h"

#include <iostream>

// How many results in a row it takes to change a backend's state. Requiring a few
// avoids flapping up/down on a single dropped probe.
static constexpr int kFailuresToMarkDown = 3;
static constexpr int kSuccessesToMarkUp = 2;

void BackendPool::add_backend(const sockaddr_in &addr)  {
    backends_.emplace_back(addr, 0);
}

Backend* BackendPool::choose_backend() {
    Backend* chosen_backend = nullptr;
    for (Backend& b: backends_) {
        if (!b.healthy) {
            continue;
        }
        if (chosen_backend == nullptr) {
            chosen_backend = &b;
            continue;
        }

        if (b.active_connections < chosen_backend->active_connections) {
            chosen_backend = &b;
        }
    }
    return chosen_backend;
}

void BackendPool::record_result(Backend& backend, bool ok) {
    if (ok) {
        backend.consecutive_failures = 0;
        backend.consecutive_successes++;
        if (!backend.healthy && backend.consecutive_successes >= kSuccessesToMarkUp) {
            backend.healthy = true;
            std::cout << "backend port " << ntohs(backend.addr.sin_port) << " is UP" << std::endl;
        }
    } else {
        backend.consecutive_successes = 0;
        backend.consecutive_failures++;
        if (backend.healthy && backend.consecutive_failures >= kFailuresToMarkDown) {
            backend.healthy = false;
            std::cout << "backend port " << ntohs(backend.addr.sin_port) << " is DOWN" << std::endl;
        }
    }
}
