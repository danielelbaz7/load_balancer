#include "load_balancer.h"

#include <cerrno>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>

LoadBalancer::LoadBalancer(uint16_t port)
    : epfd_(epoll_create1(EPOLL_CLOEXEC)),
      listener_(port, [this](int fd) { on_new_client(fd); }) {

    if (epfd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    // Tell the listener to wake up when the listening socket's accept queue has clients.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listener_.fd();
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listener_.fd(), &ev) < 0) {
        int err = errno; // save before close() can overwrite it
        close(epfd_);    // destructor won't run if we throw, so clean up here
        throw std::system_error(err, std::generic_category(), "epoll_ctl");
    }
}
