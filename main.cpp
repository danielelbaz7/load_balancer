#include "listener.h"

#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>

// Temporary test harness: a raw epoll loop driving the Listener.
// Gets replaced by the EventLoop class next.
int main() {
    Listener listener(8080, [](int client_fd) {
        std::printf("accepted fd %d\n", client_fd);
        std::fflush(stdout);
        close(client_fd); // nothing to do with the client yet, so hang up
    });

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // Ask epoll to wake us when the listening socket has clients waiting.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listener.fd();
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener.fd(), &ev) < 0) {
        perror("epoll_ctl");
        return 1;
    }

    std::printf("listening on :8080\n");
    std::fflush(stdout);

    epoll_event events[64];
    while (true) {
        // Sleep until at least one registered fd is ready (-1 = no timeout).
        int n = epoll_wait(epfd, events, 64, -1);
        if (n < 0) {
            perror("epoll_wait");
            continue;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listener.fd()) {
                listener.on_event(events[i].events);
            }
        }
    }
}
