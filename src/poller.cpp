#include "harbor/poller.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

namespace harbor {

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

constexpr int kMaxEvents = 256;

}  // namespace

#if defined(__APPLE__)

// kqueue keeps separate read and write filters per descriptor; each is
// enabled or disabled to express interest.
Poller::Poller() : fd_(::kqueue()) {
    if (fd_ < 0) {
        fail("kqueue");
    }
}

Poller::~Poller() { ::close(fd_); }

void Poller::add(int fd, std::uint64_t token, bool wantRead, bool wantWrite) {
    update(fd, token, wantRead, wantWrite);
}

void Poller::update(int fd, std::uint64_t token, bool wantRead, bool wantWrite) {
    struct kevent changes[2];
    void* udata = reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | (wantRead ? EV_ENABLE : EV_DISABLE), 0, 0, udata);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | (wantWrite ? EV_ENABLE : EV_DISABLE), 0, 0, udata);
    if (::kevent(fd_, changes, 2, nullptr, 0, nullptr) < 0) {
        fail("kevent update");
    }
}

void Poller::remove(int fd) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(fd_, changes, 2, nullptr, 0, nullptr);  // closing the fd also removes them
}

int Poller::wait(std::vector<PollEvent>& out, int timeoutMs) {
    struct kevent events[kMaxEvents];
    timespec ts{timeoutMs / 1000, static_cast<long>(timeoutMs % 1000) * 1'000'000};
    int n = ::kevent(fd_, nullptr, 0, events, kMaxEvents, &ts);
    out.clear();
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        fail("kevent wait");
    }
    for (int i = 0; i < n; ++i) {
        const struct kevent& e = events[i];
        auto token = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(e.udata));
        bool hangup = (e.flags & (EV_EOF | EV_ERROR)) != 0;
        out.push_back(PollEvent{token, e.filter == EVFILT_READ, e.filter == EVFILT_WRITE, hangup});
    }
    return n;
}

#else

Poller::Poller() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (fd_ < 0) {
        fail("epoll_create1");
    }
}

Poller::~Poller() { ::close(fd_); }

namespace {
epoll_event makeEvent(std::uint64_t token, bool wantRead, bool wantWrite) {
    epoll_event ev{};
    // EPOLLRDHUP only while reading: it stays set after a FIN, so a half-closed
    // session would otherwise wake the loop forever. EPOLLHUP/ERR always report.
    ev.events = (wantRead ? EPOLLIN | EPOLLRDHUP : 0u) | (wantWrite ? EPOLLOUT : 0u);
    ev.data.u64 = token;
    return ev;
}
}  // namespace

void Poller::add(int fd, std::uint64_t token, bool wantRead, bool wantWrite) {
    epoll_event ev = makeEvent(token, wantRead, wantWrite);
    if (::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fail("epoll_ctl add");
    }
}

void Poller::update(int fd, std::uint64_t token, bool wantRead, bool wantWrite) {
    epoll_event ev = makeEvent(token, wantRead, wantWrite);
    if (::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        fail("epoll_ctl mod");
    }
}

void Poller::remove(int fd) {
    ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Poller::wait(std::vector<PollEvent>& out, int timeoutMs) {
    epoll_event events[kMaxEvents];
    int n = ::epoll_wait(fd_, events, kMaxEvents, timeoutMs);
    out.clear();
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        fail("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        std::uint32_t e = events[i].events;
        out.push_back(PollEvent{events[i].data.u64, (e & EPOLLIN) != 0, (e & EPOLLOUT) != 0,
                                (e & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0});
    }
    return n;
}

#endif

}  // namespace harbor
