#include "backend_pool.h"

void BackendPool::add_backend(const sockaddr_in &addr)  {
    backends_.emplace_back(addr, 0);
}

Backend* BackendPool::choose_backend() {
    Backend* chosen_backend = nullptr;
    for (Backend& b: backends_) {
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

