#include "load_balancer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <optional>
#include <string>
#include <vector>

// Parses "host:port" (e.g. "127.0.0.1:9001" or "backend1:9001") into an IPv4 address.
// getaddrinfo handles both numeric IPs and hostnames (Docker service names).
static std::optional<sockaddr_in> parse_backend(const std::string& spec) {
    size_t colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size()) {
        return std::nullopt;
    }
    std::string host = spec.substr(0, colon);
    std::string port = spec.substr(colon + 1);

    addrinfo hints{};
    hints.ai_family = AF_INET;       // IPv4 only, to match sockaddr_in
    hints.ai_socktype = SOCK_STREAM; // TCP
    addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        std::fprintf(stderr, "can't resolve %s: %s\n", spec.c_str(), gai_strerror(rc));
        return std::nullopt;
    }
    sockaddr_in addr{};
    std::memcpy(&addr, result->ai_addr, sizeof(addr));
    freeaddrinfo(result);
    return addr;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <listen_port> <host:port> [host:port ...]\n", argv[0]);
        std::fprintf(stderr, "  e.g. %s 8080 127.0.0.1:9001 127.0.0.1:9002\n", argv[0]);
        return 1;
    }

    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "invalid listen port: %s\n", argv[1]);
        return 1;
    }

    std::vector<sockaddr_in> backends;
    for (int i = 2; i < argc; ++i) {
        std::optional<sockaddr_in> addr = parse_backend(argv[i]);
        if (!addr) {
            std::fprintf(stderr, "invalid backend: %s (expected host:port)\n", argv[i]);
            return 1;
        }
        backends.push_back(*addr);
    }

    LoadBalancer lb(static_cast<uint16_t>(port), backends);
    lb.run();
}
