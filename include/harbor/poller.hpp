#pragma once

#include <cstdint>
#include <vector>

namespace harbor {

struct PollEvent {
    std::uint64_t token;
    bool readable;
    bool writable;
    bool hangup;  // peer closed or error; read until recv returns 0 or fails
};

// Readiness notification for many sockets: kqueue on macOS, epoll on Linux.
// Level-triggered, so a socket stays ready until it is drained.
class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void add(int fd, std::uint64_t token, bool wantRead, bool wantWrite);
    void update(int fd, std::uint64_t token, bool wantRead, bool wantWrite);
    void remove(int fd);
    // Fills `out` with ready sockets; waits at most timeoutMs (0 = do not wait).
    int wait(std::vector<PollEvent>& out, int timeoutMs);

private:
    int fd_;
};

}  // namespace harbor
