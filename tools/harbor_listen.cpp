// Rebuilds the book from the UDP feed and prints the top of book as it changes.
//   harbor_listen [--feed-port 9100] [--snapshot-port 9001] [--host 127.0.0.1] [--seconds N]

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <vector>
#include "harbor/feed_listener.hpp"
#include "harbor/net.hpp"
#include "tool_args.hpp"

using namespace harbor;

int main(int argc, char** argv) {
    try {
        tools::Args args(argc, argv);
        std::string host = args.str("host", "127.0.0.1");
        auto snapshotPort = static_cast<std::uint16_t>(args.num("snapshot-port", 9001));
        std::uint16_t bound = 0;
        Fd udp = openUdp(static_cast<std::uint16_t>(args.num("feed-port", 9100)), bound);
        int rcvbuf = 8 << 20;
        ::setsockopt(udp.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        FeedListener listener(BookConfig{1, 1'000'000, 1, 1'000'000},
                              [&](std::uint64_t& seq, std::vector<wire::SnapshotOrderMsg>& orders) {
                                  return fetchSnapshotTcp(host, snapshotPort, seq, orders);
                              });
        std::printf("listening for the feed on udp %u\n", bound);
        std::fflush(stdout);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(args.num("seconds", 1'000'000));
        std::optional<Price> bid, ask;
        std::vector<std::uint8_t> buf(2'048);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd p{udp.get(), POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) {
                continue;
            }
            ssize_t n = ::recv(udp.get(), buf.data(), buf.size(), 0);
            if (n <= 0) {
                continue;
            }
            listener.onPacket(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
            const auto& book = listener.book().book();
            if (book.bestBid() != bid || book.bestAsk() != ask) {
                bid = book.bestBid();
                ask = book.bestAsk();
                std::printf("seq %-8llu bid %-8s ask %-8s orders %-6zu gaps %llu snapshots %llu\n",
                            static_cast<unsigned long long>(listener.book().lastSeq()),
                            bid ? std::to_string(*bid).c_str() : "-", ask ? std::to_string(*ask).c_str() : "-",
                            book.orderCount(), static_cast<unsigned long long>(listener.gaps()),
                            static_cast<unsigned long long>(listener.snapshots()));
                std::fflush(stdout);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "harbor_listen: %s\n", e.what());
        return 1;
    }
}
