// Runs the exchange until SIGINT or SIGTERM.
//   harbor_server --dir DIR [--port 9000] [--snapshot-port 9001] [--feed-port 9100]
//                 [--feed-host 127.0.0.1] [--sync full|fsync|none]

#include <csignal>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "harbor/server.hpp"
#include "tool_args.hpp"

int main(int argc, char** argv) {
    try {
        harbor::tools::Args args(argc, argv);
        if (!args.has("dir")) {
            std::fprintf(stderr, "usage: harbor_server --dir DIR [--port P] [--snapshot-port P] [--feed-port P] "
                                 "[--feed-host H] [--sync full|fsync|none]\n");
            return 2;
        }
        // A process started in the background may inherit SIGINT as ignored, and
        // an ignored signal is discarded before sigwait can see it. Installing a
        // handler first makes both signals deliverable; blocking them in every
        // thread then routes them to the sigwait below.
        struct sigaction sa{};
        sa.sa_handler = [](int) {};
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigset_t stopSignals;
        sigemptyset(&stopSignals);
        sigaddset(&stopSignals, SIGINT);
        sigaddset(&stopSignals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);

        harbor::ServerOptions o;
        o.book = harbor::BookConfig{1, 1'000'000, 1, 1'000'000};
        o.engine.dir = args.str("dir", "");
        std::string sync = args.str("sync", "full");
        o.engine.sync = sync == "none" ? harbor::SyncMode::None
                        : sync == "fsync" ? harbor::SyncMode::Fsync
                                          : harbor::SyncMode::Full;
        o.gateway.orderPort = static_cast<std::uint16_t>(args.num("port", 9000));
        o.gateway.snapshotPort = static_cast<std::uint16_t>(args.num("snapshot-port", 9001));
        o.gateway.feedPort = static_cast<std::uint16_t>(args.num("feed-port", 0));
        o.gateway.feedHost = args.str("feed-host", "127.0.0.1");

        harbor::Server server(o);
        server.start();
        std::printf("harbor: orders on %u, snapshots on %u, feed to %s:%u, sync %s, recovered seq %llu\n",
                    server.orderPort(), server.snapshotPort(), o.gateway.feedHost.c_str(), o.gateway.feedPort,
                    sync.c_str(), static_cast<unsigned long long>(server.lastDurableSeq()));
        std::fflush(stdout);
        int sig = 0;
        sigwait(&stopSignals, &sig);
        server.stop();
        std::printf("harbor: stopped at seq %llu\n", static_cast<unsigned long long>(server.lastDurableSeq()));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "harbor_server: %s\n", e.what());
        return 1;
    }
}
