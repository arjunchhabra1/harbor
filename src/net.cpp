#include "harbor/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace harbor {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

std::uint16_t localPort(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        fail("getsockname");
    }
    return ntohs(addr.sin_port);
}

}  // namespace

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Fd::~Fd() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

sockaddr_in makeAddr(const std::string& host, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("bad IPv4 address: " + host);
    }
    return addr;
}

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fail("fcntl O_NONBLOCK");
    }
}

void setNoDelay(int fd) {
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0) {
        fail("setsockopt TCP_NODELAY");
    }
}

void setNoSigPipe(int fd) {
#if defined(__APPLE__)
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) {
        fail("setsockopt SO_NOSIGPIPE");
    }
#else
    (void)fd;
#endif
}

int sendFlags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

Fd listenTcp(std::uint16_t port, std::uint16_t& boundPort) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        fail("socket");
    }
    int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr = makeAddr("127.0.0.1", port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        fail("bind tcp port " + std::to_string(port));
    }
    if (::listen(fd.get(), 128) < 0) {
        fail("listen");
    }
    setNonBlocking(fd.get());
    boundPort = localPort(fd.get());
    return fd;
}

Fd connectTcp(const std::string& host, std::uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        fail("socket");
    }
    sockaddr_in addr = makeAddr(host, port);
    int rc = 0;
    do {
        rc = ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        fail("connect " + host + ":" + std::to_string(port));
    }
    setNoDelay(fd.get());
    setNoSigPipe(fd.get());
    return fd;
}

Fd openUdp(std::uint16_t port, std::uint16_t& boundPort) {
    Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fd.valid()) {
        fail("socket");
    }
    sockaddr_in addr = makeAddr("127.0.0.1", port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        fail("bind udp port " + std::to_string(port));
    }
    boundPort = localPort(fd.get());
    return fd;
}

}  // namespace harbor
