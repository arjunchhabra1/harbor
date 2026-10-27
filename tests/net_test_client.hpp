#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include "harbor/net.hpp"
#include "harbor/protocol.hpp"

namespace harbor::testing {

// A blocking TCP client for tests: sends messages and waits for replies with a deadline.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) : fd_(connectTcp("127.0.0.1", port)) {}

    template <class M>
    void send(const M& msg) {
        std::vector<std::uint8_t> buf;
        wire::encode(msg, buf);
        sendRaw(buf);
    }

    void sendRaw(const std::vector<std::uint8_t>& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            ssize_t n = ::send(fd_.get(), bytes.data() + sent, bytes.size() - sent, sendFlags());
            if (n <= 0) {
                return;  // peer closed; tests check for that separately
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    // Waits for the next frame; false on timeout, close, or a different type.
    template <class M>
    bool expect(wire::MsgType type, M& out, int timeoutMs = 3'000) {
        wire::Frame frame{};
        if (!nextFrame(frame, timeoutMs) || frame.type != type) {
            return false;
        }
        return wire::decode(frame.payload, out);
    }

    // Next frame of any type, or nullopt on timeout/close.
    std::optional<wire::MsgType> nextType(std::vector<std::uint8_t>& payload, int timeoutMs = 3'000) {
        wire::Frame frame{};
        if (!nextFrame(frame, timeoutMs)) {
            return std::nullopt;
        }
        payload.assign(frame.payload.begin(), frame.payload.end());
        return frame.type;
    }

    // True if the server closes the connection within the timeout (after any queued frames).
    bool closedWithin(int timeoutMs = 3'000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::uint8_t buf[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd p{fd_.get(), POLLIN, 0};
            if (::poll(&p, 1, 50) > 0) {
                ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    return true;
                }
            }
        }
        return false;
    }

    int fd() const { return fd_.get(); }
    void close() { fd_ = Fd(); }

private:
    bool nextFrame(wire::Frame& frame, int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto st = reader_.next(frame);
            if (st == wire::FrameReader::Status::Frame) {
                return true;
            }
            if (st == wire::FrameReader::Status::Error) {
                return false;
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            pollfd p{fd_.get(), POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) {
                return false;
            }
            std::uint8_t buf[65536];
            ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
            if (n <= 0) {
                return false;
            }
            reader_.feed(buf, static_cast<std::size_t>(n));
        }
    }

    Fd fd_;
    wire::FrameReader reader_;
};

}  // namespace harbor::testing
