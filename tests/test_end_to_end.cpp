#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>
#include "harbor/server.hpp"
#include "net_test_client.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using namespace harbor::wire;
using harbor::testing::TestClient;
using namespace std::chrono_literals;

TEST(EndToEnd, ManyClientsTrade) {
    harbor::testing::TempDir dir;
    ServerOptions o;
    o.book = BookConfig{100, 200, 1, 65'536};
    o.engine.dir = dir.path();
    o.engine.maxBatch = 1'024;
    o.engine.maxDelay = 200us;
    o.engine.sync = SyncMode::None;
    Server server(o);
    server.start();

    constexpr int kClients = 8;
#ifdef HARBOR_SANITIZED
    constexpr std::uint32_t kCommands = 1'000;
#else
    constexpr std::uint32_t kCommands = 5'000;
#endif
    struct Seen {
        std::vector<ResponseMsg> responses;
        std::vector<ExecutionMsg> executions;
    };
    std::vector<Seen> seen(kClients);
    std::vector<std::thread> threads;
    for (int k = 0; k < kClients; ++k) {
        threads.emplace_back([&, k] {
            TestClient c(server.orderPort());
            c.send(Login{static_cast<ClientId>(k + 1)});
            LoginAccepted ok{};
            if (!c.expect(MsgType::LoginAccepted, ok)) {
                return;
            }
            std::mt19937 rng(static_cast<unsigned>(k));
            for (std::uint32_t i = 0; i < kCommands; ++i) {
                if (i > 10 && rng() % 5 == 0) {
                    c.send(CancelMsg{static_cast<std::uint32_t>(rng() % i)});
                } else {
                    Side side = rng() % 2 ? Side::Buy : Side::Sell;
                    Price price = 150 + static_cast<Price>(rng() % 11) - 5;
                    c.send(NewOrderMsg{i, side, rng() % 4 ? TimeInForce::GTC : TimeInForce::IOC, price, 1 + rng() % 10});
                }
            }
            std::vector<std::uint8_t> payload;
            // Read every response, then any executions that trail them.
            while (seen[k].responses.size() < kCommands) {
                auto type = c.nextType(payload, 10'000);
                if (!type) {
                    return;
                }
                if (*type == MsgType::Response) {
                    ResponseMsg r{};
                    decode(std::span<const std::uint8_t>(payload), r);
                    seen[k].responses.push_back(r);
                } else if (*type == MsgType::Execution) {
                    ExecutionMsg x{};
                    decode(std::span<const std::uint8_t>(payload), x);
                    seen[k].executions.push_back(x);
                }
            }
            while (auto type = c.nextType(payload, 1'000)) {
                if (*type == MsgType::Execution) {
                    ExecutionMsg x{};
                    decode(std::span<const std::uint8_t>(payload), x);
                    seen[k].executions.push_back(x);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    server.stop();

    std::set<std::uint64_t> seqs;
    std::map<std::uint64_t, std::vector<ExecutionMsg>> byMatch;
    for (int k = 0; k < kClients; ++k) {
        ASSERT_EQ(seen[k].responses.size(), kCommands) << "client " << k + 1;
        for (const auto& r : seen[k].responses) {
            EXPECT_TRUE(seqs.insert(r.seq).second) << "duplicate response seq " << r.seq;
        }
        for (const auto& x : seen[k].executions) {
            byMatch[x.matchId].push_back(x);
        }
    }
    EXPECT_EQ(seqs.size(), std::size_t{kClients} * kCommands);
    ASSERT_FALSE(byMatch.empty());
    for (const auto& [match, xs] : byMatch) {
        ASSERT_EQ(xs.size(), 2u) << "match " << match;  // exactly one report to each party
        EXPECT_NE(xs[0].role, xs[1].role) << "match " << match;
        EXPECT_EQ(xs[0].price, xs[1].price);
        EXPECT_EQ(xs[0].qty, xs[1].qty);
        EXPECT_EQ(xs[0].seq, xs[1].seq);
    }
    EXPECT_EQ(byMatch.rbegin()->first, byMatch.size());  // match ids 1..N with none missing
}
