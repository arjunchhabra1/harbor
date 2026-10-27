#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
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
