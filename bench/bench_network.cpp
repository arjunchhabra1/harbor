// Network benchmarks on localhost: order round trip by sync mode, pipelined
// throughput, order-to-feed latency, and the SPSC ring on its own.
// Build in Release: cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include "harbor/feed.hpp"
#include "harbor/server.hpp"
#include "harbor/spsc_ring.hpp"
#include "net_test_client.hpp"
#include "temp_dir.hpp"

using namespace harbor;
using namespace harbor::wire;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

const char* name(SyncMode m) {
    switch (m) {
        case SyncMode::Full: return "Full";
        case SyncMode::Fsync: return "Fsync";
        case SyncMode::None: return "None";
    }
    return "?";
}

ServerOptions options(const std::filesystem::path& dir, SyncMode mode, std::chrono::microseconds maxDelay) {
    ServerOptions o;
    o.book = BookConfig{1, 1'000'000, 1, 1'000'000};
    o.engine.dir = dir;
    o.engine.maxBatch = 1'024;
    o.engine.maxDelay = maxDelay;
    o.engine.sync = mode;
    o.engine.snapshotEvery = 1'000'000'000;
    return o;
}

void printPercentiles(const char* label, std::vector<double>& micros) {
    std::sort(micros.begin(), micros.end());
    auto at = [&](double p) { return micros[static_cast<std::size_t>(p * static_cast<double>(micros.size() - 1))]; };
    std::printf("%-34s p50 %8.1f us   p99 %8.1f us   p99.9 %8.1f us   (%zu samples)\n", label, at(0.50), at(0.99),
                at(0.999), micros.size());
}

// One order at a time: send, wait for its durable response. maxDelay 0 commits
// as soon as the engine is idle, so this measures one order's full trip.
void roundTrip(SyncMode mode, std::uint32_t samples) {
    harbor::testing::TempDir dir;
    Server server(options(dir.path(), mode, 0us));
    server.start();
    harbor::testing::TestClient c(server.orderPort());
    c.send(Login{1});
    LoginAccepted ok{};
    c.expect(MsgType::LoginAccepted, ok);
    std::vector<double> micros;
    micros.reserve(samples);
    ResponseMsg r{};
    for (std::uint32_t i = 0; i < samples + 200; ++i) {
        auto start = Clock::now();
        c.send(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, 1 + static_cast<Price>(i % 1'000), 1});
        if (!c.expect(MsgType::Response, r)) {
            std::printf("round trip: no response\n");
            return;
        }
        if (i >= 200) {  // warm-up
            micros.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
    }
    server.stop();
    char label[64];
    std::snprintf(label, sizeof label, "round trip, sync %s", name(mode));
    printPercentiles(label, micros);
}

// Several clients each pipeline orders and count responses.
void throughput(SyncMode mode, int clients, std::uint32_t perClient) {
    harbor::testing::TempDir dir;
    Server server(options(dir.path(), mode, 100us));
    server.start();
    std::atomic<std::uint64_t> responses{0};
    auto start = Clock::now();
    std::vector<std::thread> threads;
    for (int k = 0; k < clients; ++k) {
        threads.emplace_back([&, k] {
            harbor::testing::TestClient c(server.orderPort());
            c.send(Login{static_cast<ClientId>(k + 1)});
            LoginAccepted ok{};
            c.expect(MsgType::LoginAccepted, ok);
            std::thread writer([&] {
                std::mt19937 rng(static_cast<unsigned>(k));
                std::vector<std::uint8_t> buf;
                for (std::uint32_t i = 0; i < perClient; ++i) {
                    Side side = rng() % 2 ? Side::Buy : Side::Sell;
                    encode(NewOrderMsg{i, side, TimeInForce::GTC, 500'000 + static_cast<Price>(rng() % 101) - 50,
                                       static_cast<Quantity>(1 + rng() % 10)},
                           buf);
                    if (buf.size() > 64 * 1024) {
                        c.sendRaw(buf);
                        buf.clear();
                    }
                }
                c.sendRaw(buf);
            });
            std::vector<std::uint8_t> payload;
            std::uint32_t mine = 0;
            while (mine < perClient) {
                auto type = c.nextType(payload, 30'000);
                if (!type) {
                    break;
                }
                if (*type == MsgType::Response) {
                    ++mine;
                }
            }
            responses += mine;
            writer.join();
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    double secs = std::chrono::duration<double>(Clock::now() - start).count();
    server.stop();
    std::printf("throughput, sync %-5s %d clients     %10.0f orders/s   (%llu responses in %.2f s)\n", name(mode),
                clients, static_cast<double>(responses.load()) / secs, static_cast<unsigned long long>(responses.load()),
                secs);
}

// Time from sending an order until its Added event arrives on the UDP feed.
void feedLag(std::uint32_t samples) {
    harbor::testing::TempDir dir;
    std::uint16_t feedPort = 0;
    Fd udp = openUdp(0, feedPort);
    ServerOptions o = options(dir.path(), SyncMode::None, 0us);
    o.gateway.feedPort = feedPort;
    Server server(o);
    server.start();
    harbor::testing::TestClient c(server.orderPort());
    c.send(Login{1});
    LoginAccepted ok{};
    c.expect(MsgType::LoginAccepted, ok);
    std::vector<double> micros;
    std::vector<std::uint8_t> buf(2'048);
    std::vector<FeedEvent> events;
    for (std::uint32_t i = 0; i < samples + 200; ++i) {
        OrderId want = exchangeId(1, i);
        auto start = Clock::now();
        c.send(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, 1 + static_cast<Price>(i % 1'000), 1});
        bool seen = false;
        while (!seen) {
            pollfd p{udp.get(), POLLIN, 0};
            if (::poll(&p, 1, 1'000) <= 0) {
                std::printf("feed lag: packet lost, skipping sample\n");
                break;
            }
            ssize_t n = ::recv(udp.get(), buf.data(), buf.size(), 0);
            std::uint32_t session = 0;
            if (n > 0 && decodePacket(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)), session,
                                      events)) {
                for (const auto& e : events) {
                    seen = seen || (e.type == FeedEvent::Type::Added && e.id == want);
                }
            }
        }
        if (seen && i >= 200) {
            micros.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
        ResponseMsg r{};
        c.expect(MsgType::Response, r);
    }
    server.stop();
    printPercentiles("order to feed event, sync None", micros);
}

void ringPingPong(std::uint32_t roundTrips) {
    auto there = std::make_unique<SpscRing<std::uint64_t, 1024>>();
    auto back = std::make_unique<SpscRing<std::uint64_t, 1024>>();
    std::thread echo([&] {
        std::uint64_t v = 0;
        for (std::uint32_t i = 0; i < roundTrips; ++i) {
            while (!there->tryPop(v)) {
            }
            while (!back->tryPush(v)) {
            }
        }
    });
    auto start = Clock::now();
    std::uint64_t v = 0;
    for (std::uint32_t i = 0; i < roundTrips; ++i) {
        while (!there->tryPush(i)) {
        }
        while (!back->tryPop(v)) {
        }
    }
    double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    echo.join();
    std::printf("SPSC ring ping-pong                %8.0f ns per round trip (%u round trips)\n",
                ns / roundTrips, roundTrips);
}

void ringThroughput(std::uint64_t items) {
    auto ring = std::make_unique<SpscRing<std::uint64_t, 65'536>>();
    auto start = Clock::now();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < items; ++i) {
            while (!ring->tryPush(i)) {
            }
        }
    });
    std::uint64_t v = 0, sum = 0;
    for (std::uint64_t i = 0; i < items; ++i) {
        while (!ring->tryPop(v)) {
        }
        sum += v;
    }
    producer.join();
    double secs = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("SPSC ring one-way                  %8.1f M items/s (checksum %llu)\n",
                static_cast<double>(items) / secs / 1e6, static_cast<unsigned long long>(sum % 1000));
}

}  // namespace

int main() {
    ringPingPong(1'000'000);
    ringThroughput(100'000'000);
    roundTrip(SyncMode::None, 20'000);
    roundTrip(SyncMode::Fsync, 20'000);
    roundTrip(SyncMode::Full, 2'000);
    throughput(SyncMode::None, 4, 200'000);
    throughput(SyncMode::Fsync, 4, 200'000);
    feedLag(20'000);
    return 0;
}
