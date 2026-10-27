#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace harbor {

// Owning socket descriptor; closes on destruction. Every failure in this
// header throws std::runtime_error.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// Nonblocking TCP listener on 127.0.0.1; port 0 picks a free port.
Fd listenTcp(std::uint16_t port, std::uint16_t& boundPort);
// Blocking connect to host:port (an IPv4 address), with TCP_NODELAY.
Fd connectTcp(const std::string& host, std::uint16_t port);
// UDP socket bound to 127.0.0.1:port; port 0 picks a free port.
Fd openUdp(std::uint16_t port, std::uint16_t& boundPort);

sockaddr_in makeAddr(const std::string& host, std::uint16_t port);
void setNonBlocking(int fd);
void setNoDelay(int fd);
// Writes to a closed peer must not raise SIGPIPE: SO_NOSIGPIPE on macOS,
// MSG_NOSIGNAL (sendFlags) on Linux.
void setNoSigPipe(int fd);
int sendFlags();

}  // namespace harbor
