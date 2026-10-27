#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include "harbor/engine_loop.hpp"
#include "harbor/gateway.hpp"
#include "net_test_client.hpp"
#include "temp_dir.hpp"
using namespace harbor;
using namespace harbor::wire;
using harbor::testing::TestClient;
using namespace std::chrono_literals;

namespace {

constexpr BookConfig kCfg{100, 200, 1, 1'024};

// Gateway and EngineLoop pumped alternately on one background thread.
class GatewayTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;
    std::unique_ptr<InboundRing> inbound = std::make_unique<InboundRing>();
    std::unique_ptr<OutboundRing> outbound = std::make_unique<OutboundRing>();
    std::unique_ptr<EngineLoop> loop;
    std::unique_ptr<Gateway> gateway;
    std::atomic<bool> stop{false};
    std::thread pump;

    void SetUp() override {
        EngineOptions eo;
        eo.dir = dir.path();
        eo.maxBatch = 1'000;
        eo.maxDelay = 1ms;
        eo.sync = SyncMode::None;
        loop = std::make_unique<EngineLoop>(kCfg, eo, *inbound, *outbound);
        GatewayOptions go;
        gateway = std::make_unique<Gateway>(go, *inbound, *outbound, [this] {
            return std::pair{loop->lastDurableSeq(), loop->lastFeedSeq()};
        });
        pump = std::thread([this] {
            while (!stop.load()) {
                gateway->runOnce(1);
                loop->runOnce(std::chrono::steady_clock::now());
            }
        });
    }

    void TearDown() override {
        stop = true;
        pump.join();
    }

    std::unique_ptr<TestClient> login(ClientId id) {
        auto c = std::make_unique<TestClient>(gateway->orderPort());
        c->send(Login{id});
        LoginAccepted ok{};
        EXPECT_TRUE(c->expect(MsgType::LoginAccepted, ok)) << "client " << id;
        return c;
    }
};

}  // namespace

TEST_F(GatewayTest, LoginAccepted) {
    TestClient c(gateway->orderPort());
    c.send(Login{1});
    LoginAccepted ok{};
    ASSERT_TRUE(c.expect(MsgType::LoginAccepted, ok));
    EXPECT_EQ(ok, (LoginAccepted{0, 0}));
}

TEST_F(GatewayTest, DuplicateLoginRejected) {
    auto first = login(1);
    TestClient second(gateway->orderPort());
    second.send(Login{1});
    LoginRejected rejected{};
    ASSERT_TRUE(second.expect(MsgType::LoginRejected, rejected));
    EXPECT_EQ(rejected.reason, LoginRejected::DuplicateSession);
    EXPECT_TRUE(second.closedWithin());
    first->send(NewOrderMsg{1, Side::Buy, TimeInForce::GTC, 150, 1});  // the first session still works
    ResponseMsg r{};
    ASSERT_TRUE(first->expect(MsgType::Response, r));
    EXPECT_EQ(r.result, Reject::None);
}

TEST_F(GatewayTest, ClientIdZeroRejected) {
    TestClient c(gateway->orderPort());
    c.send(Login{0});
    LoginRejected rejected{};
    ASSERT_TRUE(c.expect(MsgType::LoginRejected, rejected));
    EXPECT_EQ(rejected.reason, LoginRejected::Unknown);
    EXPECT_TRUE(c.closedWithin());
}

TEST_F(GatewayTest, OrderGetsDurableResponse) {
    auto c = login(1);
    c->send(NewOrderMsg{5, Side::Buy, TimeInForce::GTC, 150, 10});
    ResponseMsg r{};
    ASSERT_TRUE(c->expect(MsgType::Response, r));
    EXPECT_EQ(r, (ResponseMsg{5, 2, Reject::None, 1, 10}));
    EXPECT_EQ(loop->lastDurableSeq(), 1u);  // the response was only sent once durable
}

TEST_F(GatewayTest, CommandBeforeLoginClosesSession) {
    TestClient c(gateway->orderPort());
    c.send(NewOrderMsg{1, Side::Buy, TimeInForce::GTC, 150, 1});
    EXPECT_TRUE(c.closedWithin());
}

TEST_F(GatewayTest, MalformedFrameClosesSession) {
    auto c = login(1);
    c->sendRaw({1, 0, 77});
    EXPECT_TRUE(c->closedWithin());
}

TEST_F(GatewayTest, CancelTargetsOwnOrdersOnly) {
    auto one = login(1);
    auto two = login(2);
    one->send(NewOrderMsg{7, Side::Buy, TimeInForce::GTC, 150, 5});
    ResponseMsg r{};
    ASSERT_TRUE(one->expect(MsgType::Response, r));
    two->send(CancelMsg{7});  // client 2's id 7 is a different order: it does not exist
    ASSERT_TRUE(two->expect(MsgType::Response, r));
    EXPECT_EQ(r.result, Reject::UnknownOrder);
    one->send(CancelMsg{7});
    ASSERT_TRUE(one->expect(MsgType::Response, r));
    EXPECT_EQ(r.result, Reject::None);  // client 1's order was still resting
}

// ---- Two-thread server ----

#include "harbor/server.hpp"

namespace {

class ServerTest : public ::testing::Test {
protected:
    harbor::testing::TempDir dir;
    std::unique_ptr<Server> server;

    void start(std::size_t sendBufferLimit = std::size_t{1} << 20, int socketSendBuffer = 0,
               BookConfig cfg = BookConfig{100, 200, 1, 65'536}) {
        ServerOptions o;
        o.book = cfg;
        o.engine.dir = dir.path();
        o.engine.maxBatch = 1'024;
        o.engine.maxDelay = 200us;
        o.engine.sync = SyncMode::None;
        o.gateway.sendBufferLimit = sendBufferLimit;
        o.gateway.socketSendBuffer = socketSendBuffer;
        server = std::make_unique<Server>(o);
        server->start();
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
    }

    std::unique_ptr<TestClient> login(ClientId id) {
        // A just-closed session for the same client may not be gone yet: retry briefly.
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto c = std::make_unique<TestClient>(server->orderPort());
            c->send(Login{id});
            std::vector<std::uint8_t> payload;
            auto type = c->nextType(payload);
            if (type == MsgType::LoginAccepted) {
                return c;
            }
            std::this_thread::sleep_for(20ms);
        }
        ADD_FAILURE() << "login " << id << " never accepted";
        return nullptr;
    }
};

}  // namespace

TEST_F(ServerTest, ExecutionsReachBothParties) {
    start();
    auto seller = login(1);
    auto buyer = login(2);
    seller->send(NewOrderMsg{11, Side::Sell, TimeInForce::GTC, 150, 5});
    ResponseMsg r{};
    ASSERT_TRUE(seller->expect(MsgType::Response, r));
    buyer->send(NewOrderMsg{22, Side::Buy, TimeInForce::GTC, 150, 5});
    ASSERT_TRUE(buyer->expect(MsgType::Response, r));
    EXPECT_EQ(r, (ResponseMsg{22, 2, Reject::None, 2, 0}));
    ExecutionMsg mine{}, theirs{};
    ASSERT_TRUE(buyer->expect(MsgType::Execution, mine));
    ASSERT_TRUE(seller->expect(MsgType::Execution, theirs));
    EXPECT_EQ(mine, (ExecutionMsg{22, 2, 150, 5, ExecutionMsg::Aggressor, 1}));
    EXPECT_EQ(theirs, (ExecutionMsg{11, 2, 150, 5, ExecutionMsg::Resting, 1}));
}

TEST_F(ServerTest, ResponsesForDisconnectedClientAreDropped) {
    start();
    {
        auto c = login(1);
        for (std::uint32_t i = 0; i < 100; ++i) {
            c->send(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, 120, 1});
        }
    }  // closes with responses in flight
    auto again = login(1);
    again->send(NewOrderMsg{1'000, Side::Buy, TimeInForce::GTC, 121, 1});
    ResponseMsg r{};
    ASSERT_TRUE(again->expect(MsgType::Response, r));  // the first frame is for its own command
    EXPECT_EQ(r.clientOrderId, 1'000u);
    std::vector<std::uint8_t> payload;
    EXPECT_FALSE(again->nextType(payload, 300).has_value());  // and nothing stale follows
}

TEST_F(ServerTest, SlowClientDisconnected) {
    // Bound kernel buffering on both ends; otherwise Linux autotunes loopback
    // buffers to several MB and absorbs everything the slow client ignores.
    start(64 * 1024, 64 * 1024);
    auto slow = login(1);
    int rcvbuf = 64 * 1024;
    ::setsockopt(slow->fd(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    auto fast = login(2);
    // 50,000 responses (over 1 MB) that the slow client never reads: far more
    // than the bounded socket buffers plus the 64 KiB gateway limit.
    constexpr std::uint32_t kOrders = 50'000;
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        slow->send(NewOrderMsg{i, Side::Sell, TimeInForce::GTC, 150, 1});
    }
    for (int i = 0; i < 1'000 && server->sessionCount() != 1; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(server->sessionCount(), 1u);  // the slow reader was cut off
    // The fast client is unaffected: it trades against what the slow one left resting.
    fast->send(NewOrderMsg{1, Side::Buy, TimeInForce::IOC, 150, 1'000});
    ResponseMsg r{};
    ASSERT_TRUE(fast->expect(MsgType::Response, r, 10'000));
    EXPECT_EQ(r.result, Reject::None);
    std::uint32_t filled = 0;
    ExecutionMsg x{};
    while (filled < 1'000 && fast->expect(MsgType::Execution, x, 2'000)) {
        filled += x.qty;
    }
    EXPECT_EQ(filled, 1'000u);  // resting orders outlive their owner's session
}

TEST_F(ServerTest, SurvivesResetsWithOutputPending) {
    start();
    std::vector<std::thread> clients;
    for (ClientId k = 1; k <= 8; ++k) {
        clients.emplace_back([&, k] {
            TestClient c(server->orderPort());
            c.send(Login{k});
            for (std::uint32_t i = 0; i < 2'000; ++i) {
                c.send(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, 120, 1});
            }
            linger reset{1, 0};  // close with RST while responses are still being sent
            ::setsockopt(c.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof reset);
        });
    }
    for (auto& t : clients) {
        t.join();
    }
    auto after = login(50);  // the server is still alive and serving
    ASSERT_NE(after, nullptr);
    after->send(NewOrderMsg{1, Side::Buy, TimeInForce::GTC, 121, 1});
    ResponseMsg r{};
    ASSERT_TRUE(after->expect(MsgType::Response, r));
    EXPECT_EQ(r.result, Reject::None);
}

TEST_F(ServerTest, HalfClosedClientStillGetsResponses) {
    start();
    TestClient c(server->orderPort());
    c.send(Login{7});
    for (std::uint32_t i = 0; i < 10; ++i) {
        c.send(NewOrderMsg{i, Side::Buy, TimeInForce::GTC, 120, 1});
    }
    ::shutdown(c.fd(), SHUT_WR);  // "that's all I'm sending" (FIN), still reading
    LoginAccepted ok{};
    ASSERT_TRUE(c.expect(MsgType::LoginAccepted, ok));
    for (std::uint32_t i = 0; i < 10; ++i) {
        ResponseMsg r{};
        ASSERT_TRUE(c.expect(MsgType::Response, r)) << "response " << i;
        EXPECT_EQ(r.clientOrderId, i);
        EXPECT_EQ(r.result, Reject::None);
    }
    EXPECT_TRUE(c.closedWithin());  // then the server finishes the session
}
