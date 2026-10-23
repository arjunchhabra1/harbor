#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>
#include "harbor/bytes.hpp"
#include "harbor/command.hpp"
#include "harbor/engine.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/reference_book.hpp"
#include "op_generator.hpp"

namespace harbor::testing {

inline constexpr BookConfig kCrashCfg{1'000, 2'000, 1, 4096};

struct CrashResult {
    std::uint64_t reportedDurable = 0;   // highest seq the child reported as durable
    std::uint64_t recoveredDurable = 0;  // seq recovered after the kill
    bool stateMatches = false;           // recovered book == reference at recoveredDurable
    std::string error;                   // non-empty if recovery threw

    bool ok() const { return error.empty() && stateMatches && recoveredDurable >= reportedDurable; }
};

inline EngineOptions crashOptions(const std::filesystem::path& dir) {
    EngineOptions o;
    o.dir = dir;
    o.maxBatch = 64;
    o.snapshotEvery = 5'000;
    o.sync = SyncMode::None;  // a SIGKILL does not lose the page cache
    return o;
}

inline OpGenerator crashGenerator(std::uint64_t seed) {
    return OpGenerator(seed, 1'500, 100, kCrashCfg.minPrice, kCrashCfg.maxPrice);
}

// Child process body: run the engine until killed, reporting lastDurableSeq
// (8 bytes, little-endian) on `fd` after every commit. Never returns.
[[noreturn]] inline void runCrashChild(const std::filesystem::path& dir, std::uint64_t seed, int fd) {
    try {
        Engine<FastBook> engine(kCrashCfg, crashOptions(dir));
        OpGenerator gen = crashGenerator(seed);
        std::chrono::steady_clock::time_point now{};
        std::uint64_t reported = 0;
        for (std::uint64_t i = 0; i < 50'000'000; ++i) {
            now += std::chrono::microseconds(1);
            engine.submit(gen.next([&](OrderId id) { return engine.book().contains(id); }), now);
            engine.drain([](const Output&) {});
            if (engine.lastDurableSeq() != reported) {
                reported = engine.lastDurableSeq();
                std::uint8_t buf[8];
                putU64(buf, reported);
                if (::write(fd, buf, sizeof buf) != static_cast<ssize_t>(sizeof buf)) {
                    ::_exit(3);
                }
            }
        }
        ::_exit(0);
    } catch (...) {
        ::_exit(2);
    }
}

template <class Book>
std::vector<std::tuple<OrderId, ClientId, Side, Price, Quantity>> restingOf(const Book& book) {
    std::vector<std::tuple<OrderId, ClientId, Side, Price, Quantity>> out;
    book.forEachResting([&](OrderId id, ClientId c, Side s, Price p, Quantity q) {
        out.emplace_back(id, c, s, p, q);
    });
    return out;
}

// Recovers `dir` and compares it with a reference book that applied the first
// recoveredDurable commands of the seeded stream.
inline void checkRecovered(const std::filesystem::path& dir, std::uint64_t seed, CrashResult& result) {
    try {
        Engine<FastBook> engine(kCrashCfg, crashOptions(dir));
        result.recoveredDurable = engine.lastDurableSeq();
        ReferenceBook ref{kCrashCfg};
        OpGenerator gen = crashGenerator(seed);
        std::vector<Fill> fills;
        for (std::uint64_t i = 0; i < result.recoveredDurable; ++i) {
            applyCommand(ref, gen.next([&](OrderId id) { return ref.contains(id); }), fills);
        }
        std::string e;
        result.stateMatches = restingOf(engine.book()) == restingOf(ref) &&
                              engine.book().bestBid() == ref.bestBid() &&
                              engine.book().bestAsk() == ref.bestAsk() && engine.book().checkInvariants(e);
    } catch (const std::exception& ex) {
        result.error = ex.what();
    }
}

// Forks a child that runs the engine on `dir`, waits until it reports at
// least killAfterSeq durable commands, waits extraMicros more (reading its
// reports throughout), SIGKILLs it, then recovers and checks. Keying the kill
// to the child's progress rather than wall time keeps coverage the same on
// fast machines and slow sanitizer builds.
inline CrashResult runCrashTrial(const std::filesystem::path& dir, std::uint64_t seed,
                                 std::uint64_t killAfterSeq, std::uint32_t extraMicros) {
    CrashResult result;
    int fds[2];
    if (::pipe(fds) != 0) {
        result.error = std::string("pipe: ") + std::strerror(errno);
        return result;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        result.error = std::string("fork: ") + std::strerror(errno);
        ::close(fds[0]);
        ::close(fds[1]);
        return result;
    }
    if (pid == 0) {
        ::close(fds[0]);
        runCrashChild(dir, seed, fds[1]);
    }
    ::close(fds[1]);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);

    std::vector<std::uint8_t> pending;
    auto readReports = [&] {
        std::uint8_t buf[4096];
        for (;;) {
            ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n <= 0) {
                break;
            }
            pending.insert(pending.end(), buf, buf + n);
        }
        std::size_t whole = pending.size() / 8 * 8;
        for (std::size_t i = 0; i < whole; i += 8) {
            result.reportedDurable = std::max(result.reportedDurable, getU64(pending.data() + i));
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(whole));
    };

    auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (result.reportedDurable < killAfterSeq && std::chrono::steady_clock::now() < giveUp) {
        readReports();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    bool reachedTarget = result.reportedDurable >= killAfterSeq;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(extraMicros);
    while (std::chrono::steady_clock::now() < deadline) {
        readReports();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    readReports();  // anything written before the kill
    ::close(fds[0]);

    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
        result.error = "child exited before the kill (status " + std::to_string(status) + ")";
        return result;
    }
    if (!reachedTarget) {
        result.error = "child reported only " + std::to_string(result.reportedDurable) + " of " +
                       std::to_string(killAfterSeq) + " durable commands within 60 s";
        return result;
    }
    checkRecovered(dir, seed, result);
    return result;
}

}  // namespace harbor::testing
