#include "harbor/server.hpp"

#include <chrono>
#include "harbor/feed_listener.hpp"

namespace harbor {

namespace {

// Each start is a new feed session: feed seqs restart with the recovered
// engine, so listeners must be able to tell this run's seqs from the last one's.
GatewayOptions withNewFeedSession(const ServerOptions& options) {
    GatewayOptions g = options.gateway;
    g.feedSession = nextFeedSession(options.engine.dir, options.engine.sync);
    return g;
}

}  // namespace

Server::Server(const ServerOptions& options)
    : inbound_(std::make_unique<InboundRing>()),
      outbound_(std::make_unique<OutboundRing>()),
      loop_(options.book, options.engine, *inbound_, *outbound_),
      gateway_(withNewFeedSession(options), *inbound_, *outbound_,
               [this] { return std::pair{loop_.lastDurableSeq(), loop_.lastFeedSeq()}; }) {}

Server::~Server() { stop(); }

void Server::start() {
    if (running_) {
        return;
    }
    running_ = true;
    engine_ = std::thread([this] {
        // Spin while there is work; back off when idle so an idle exchange
        // does not burn a core.
        int idle = 0;
        while (loop_.runOnce(EngineLoop::Clock::now())) {
            if (loop_.busy()) {
                idle = 0;
            } else if (++idle < 1'000) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        engineDone_.store(true, std::memory_order_release);
    });
    network_ = std::thread([this] {
        while (!engineDone_.load(std::memory_order_acquire)) {
            gateway_.runOnce(1);
        }
        gateway_.runOnce(0);  // deliver what the engine released while stopping
        gateway_.runOnce(0);
    });
}

void Server::stop() {
    if (!running_) {
        return;
    }
    gateway_.requestStop();
    engine_.join();
    network_.join();
    running_ = false;
}

}  // namespace harbor
