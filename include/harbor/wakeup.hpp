#pragma once

#include <atomic>
#include "harbor/net.hpp"

namespace harbor {

// Lets the engine thread wake the network thread out of Poller::wait when it
// has pushed output. The sleeper sets `sleeping`, fences, then re-checks for
// work; the notifier publishes work, fences, then writes to the pipe only if
// the sleeper is (still) asleep. With both fences seq_cst, at least one side
// sees the other, so a wakeup is never lost and a busy sleeper costs nothing.
class Wakeup {
public:
    Wakeup();

    int readFd() const { return read_.get(); }

    // Sleeper side.
    void beginSleep() {
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    void endSleep() { sleeping_.store(false, std::memory_order_relaxed); }
    void drain();  // empty the pipe after a wakeup

    // Notifier side: call after publishing work.
    void notify() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_relaxed) && sleeping_.exchange(false, std::memory_order_relaxed)) {
            signal();
        }
    }

private:
    void signal();

    Fd read_;
    Fd write_;
    std::atomic<bool> sleeping_{false};
};

}  // namespace harbor
