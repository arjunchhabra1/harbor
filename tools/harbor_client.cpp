// Sends orders to the exchange and prints what comes back.
//   harbor_client [--port 9000] --client N [--id ID] buy|sell PRICE QTY [ioc]
//   harbor_client [--port 9000] --client N [--id ID] cancel
//   harbor_client [--port 9000] --client N [--id ID] replace PRICE QTY
//   harbor_client [--port 9000] --client N --replay COUNT [--seed S]

#include <chrono>
#include <cstdio>
#include <exception>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <vector>
#include "harbor/net.hpp"
#include "harbor/protocol.hpp"
#include "tool_args.hpp"

using namespace harbor;
using namespace harbor::wire;

namespace {

void sendAll(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, sendFlags());
        if (n <= 0) {
            throw std::runtime_error("connection closed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

// Reads frames until `idleMs` passes without one; calls f for each.
template <class F>
void readFrames(int fd, FrameReader& reader, int idleMs, F&& f) {
    std::vector<std::uint8_t> buf(65'536);
    for (;;) {
        Frame frame{};
        while (reader.next(frame) == FrameReader::Status::Frame) {
            if (!f(frame)) {
                return;
            }
        }
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, idleMs) <= 0) {
            return;
        }
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            return;
        }
        reader.feed(buf.data(), static_cast<std::size_t>(n));
    }
}

void print(const Frame& frame) {
    if (frame.type == MsgType::Response) {
        ResponseMsg r{};
        decode(frame.payload, r);
        std::printf("response   id %u  %s  seq %llu  resting %u\n", r.clientOrderId, toString(r.result),
                    static_cast<unsigned long long>(r.seq), r.restingQty);
    } else if (frame.type == MsgType::Execution) {
        ExecutionMsg x{};
        decode(frame.payload, x);
        std::printf("execution  id %u  %u @ %lld  %s  match %llu\n", x.clientOrderId, x.qty,
                    static_cast<long long>(x.price), x.role == ExecutionMsg::Aggressor ? "aggressor" : "resting",
                    static_cast<unsigned long long>(x.matchId));
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        tools::Args args(argc, argv);
        if (!args.has("client")) {
            std::fprintf(stderr, "usage: harbor_client [--port P] --client N [--id ID] buy|sell PRICE QTY [ioc] | "
                                 "cancel | replace PRICE QTY | --replay COUNT [--seed S]\n");
            return 2;
        }
        Fd fd = connectTcp(args.str("host", "127.0.0.1"), static_cast<std::uint16_t>(args.num("port", 9000)));
        FrameReader reader;
        std::vector<std::uint8_t> out;
        encode(Login{static_cast<std::uint32_t>(args.num("client", 0))}, out);
        sendAll(fd.get(), out);
        bool loggedIn = false;
        readFrames(fd.get(), reader, 3'000, [&](const Frame& f) {
            if (f.type == MsgType::LoginAccepted) {
                LoginAccepted ok{};
                decode(f.payload, ok);
                std::printf("logged in  durable seq %llu  feed seq %llu\n",
                            static_cast<unsigned long long>(ok.lastDurableSeq),
                            static_cast<unsigned long long>(ok.feedSeq));
                loggedIn = true;
            } else {
                std::printf("login rejected\n");
            }
            return false;
        });
        if (!loggedIn) {
            return 1;
        }

        if (args.has("replay")) {
            std::uint64_t count = args.num("replay", 0);
            std::mt19937_64 rng(args.num("seed", 1));
            out.clear();
            for (std::uint64_t i = 0; i < count; ++i) {
                Side side = rng() % 2 ? Side::Buy : Side::Sell;
                encode(NewOrderMsg{static_cast<std::uint32_t>(i), side, TimeInForce::GTC,
                                   500'000 + static_cast<Price>(rng() % 21) - 10, static_cast<Quantity>(1 + rng() % 10)},
                       out);
            }
            auto start = std::chrono::steady_clock::now();
            sendAll(fd.get(), out);
            std::uint64_t responses = 0, executions = 0;
            readFrames(fd.get(), reader, 2'000, [&](const Frame& f) {
                responses += f.type == MsgType::Response ? 1 : 0;
                executions += f.type == MsgType::Execution ? 1 : 0;
                return true;
            });
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - 2.0;
            std::printf("replayed %llu orders: %llu responses, %llu executions, ~%.0f orders/s\n",
                        static_cast<unsigned long long>(count), static_cast<unsigned long long>(responses),
                        static_cast<unsigned long long>(executions), secs > 0 ? static_cast<double>(responses) / secs : 0.0);
            return 0;
        }

        const auto& pos = args.positional;
        auto id = static_cast<std::uint32_t>(args.num("id", 1));
        out.clear();
        if (pos.size() >= 3 && (pos[0] == "buy" || pos[0] == "sell")) {
            encode(NewOrderMsg{id, pos[0] == "buy" ? Side::Buy : Side::Sell,
                               pos.size() > 3 && pos[3] == "ioc" ? TimeInForce::IOC : TimeInForce::GTC,
                               std::stoll(pos[1]), static_cast<Quantity>(std::stoul(pos[2]))},
                   out);
        } else if (pos.size() == 1 && pos[0] == "cancel") {
            encode(CancelMsg{id}, out);
        } else if (pos.size() == 3 && pos[0] == "replace") {
            encode(ReplaceMsg{id, std::stoll(pos[1]), static_cast<Quantity>(std::stoul(pos[2]))}, out);
        } else {
            std::fprintf(stderr, "unknown command\n");
            return 2;
        }
        sendAll(fd.get(), out);
        readFrames(fd.get(), reader, 500, [&](const Frame& f) {
            print(f);
            return true;
        });
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "harbor_client: %s\n", e.what());
        return 1;
    }
}
