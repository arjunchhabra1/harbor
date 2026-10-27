#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include "harbor/engine_loop.hpp"
#include "harbor/gateway.hpp"
#include "harbor/wakeup.hpp"

namespace harbor {

struct ServerOptions {
    BookConfig book;
    EngineOptions engine;
    GatewayOptions gateway;
};

// The whole exchange: a network thread running the Gateway and an engine
// thread running the EngineLoop, connected by two SPSC rings.
class Server {
public:
    explicit Server(const ServerOptions& options);  // recovers the engine directory and binds sockets
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    // Commits and delivers everything already received, then joins both threads.
    void stop();

    std::uint16_t orderPort() const { return gateway_.orderPort(); }
    std::uint16_t snapshotPort() const { return gateway_.snapshotPort(); }
    std::size_t sessionCount() const { return gateway_.sessionCount(); }
    std::uint64_t lastDurableSeq() const { return loop_.lastDurableSeq(); }
    std::uint64_t lastFeedSeq() const { return loop_.lastFeedSeq(); }

private:
    std::unique_ptr<InboundRing> inbound_;
    std::unique_ptr<OutboundRing> outbound_;
    Wakeup wakeup_;
    EngineLoop loop_;
    Gateway gateway_;
    std::thread network_;
    std::thread engine_;
    std::atomic<bool> engineDone_{false};
    bool running_ = false;
};

}  // namespace harbor
