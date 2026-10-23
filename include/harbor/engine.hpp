#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/file_io.hpp"
#include "harbor/types.hpp"
#include "harbor/wal.hpp"

namespace harbor {

struct EngineOptions {
    std::filesystem::path dir;
    std::uint32_t maxBatch = 1024;                    // commit when this many commands are pending
    std::chrono::microseconds maxDelay{1000};         // ...or the oldest has waited this long
    std::uint64_t snapshotEvery = 1'000'000;          // commands between snapshots
    SyncMode sync = SyncMode::Full;
    std::function<void(int step)> afterSnapshotStep;  // test hook: called after snapshot steps 1-4
};

struct Output {
    std::uint64_t seq;
    Reject result;
    std::span<const Fill> fills;  // valid until drain() returns
};

// Wraps a book with a write-ahead log. Every command is logged and applied
// immediately, but its output is released only after the batch containing it
// has been written and synced, so nothing is ever acknowledged that a crash
// could undo. Single-threaded; the caller supplies the time.
template <class Book>
class Engine {
public:
    using Clock = std::chrono::steady_clock;

    Engine(const BookConfig& cfg, const EngineOptions& options)
        : cfg_(cfg),
          opts_(options),
          book_(cfg),
          batch_(std::size_t{options.maxBatch} * kRecordSize) {
        if (opts_.maxBatch == 0) {
            throw std::invalid_argument("EngineOptions: maxBatch must be positive");
        }
        outputs_.reserve(opts_.maxBatch);
        fills_.reserve(std::size_t{opts_.maxBatch} * 4);
        scratch_.reserve(256);
        open();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Logs and applies one command; returns its seq. Commits when the batch is full.
    std::uint64_t submit(const Command& cmd, Clock::time_point now) {
        checkUsable();
        if (pending_ == 0) {
            batchStart_ = now;
        }
        std::uint64_t seq = ++lastSeq_;
        encodeRecord(seq, cmd, batch_.data() + pending_ * kRecordSize);
        Reject result = applyCommand(book_, cmd, scratch_);
        outputs_.push_back(PendingOutput{seq, result, fills_.size(), scratch_.size()});
        fills_.insert(fills_.end(), scratch_.begin(), scratch_.end());
        if (++pending_ == opts_.maxBatch) {
            commit();
        }
        return seq;
    }

    // Commits if the oldest pending command has waited at least maxDelay.
    void poll(Clock::time_point now) {
        checkUsable();
        if (pending_ > 0 && now - batchStart_ >= opts_.maxDelay) {
            commit();
        }
    }

    // Writes and syncs every pending command, then releases their outputs.
    void commit() {
        checkUsable();
        if (pending_ == 0) {
            return;
        }
        try {
            wal_->writeAll(batch_.data(), pending_ * kRecordSize);
            wal_->sync(opts_.sync);
        } catch (...) {
            // The book is now ahead of the durable log; only recovery can fix that.
            broken_ = true;
            throw;
        }
        durableSeq_ = lastSeq_;
        pending_ = 0;
        released_ = outputs_.size();
    }

    // Calls onOutput(const Output&) for each released output in seq order.
    template <class F>
    void drain(F&& onOutput) {
        for (std::size_t i = 0; i < released_; ++i) {
            const PendingOutput& p = outputs_[i];
            onOutput(Output{p.seq, p.result, std::span<const Fill>(fills_.data() + p.fillBegin, p.fillCount)});
        }
        if (released_ == outputs_.size()) {
            outputs_.clear();
            fills_.clear();
        } else {
            // Keep outputs submitted after the last commit; shift them to the front.
            std::size_t fillShift = outputs_[released_].fillBegin;
            fills_.erase(fills_.begin(), fills_.begin() + static_cast<std::ptrdiff_t>(fillShift));
            outputs_.erase(outputs_.begin(), outputs_.begin() + static_cast<std::ptrdiff_t>(released_));
            for (PendingOutput& p : outputs_) {
                p.fillBegin -= fillShift;
            }
        }
        released_ = 0;
    }

    const Book& book() const { return book_; }
    std::uint64_t lastSeq() const { return lastSeq_; }
    std::uint64_t lastDurableSeq() const { return durableSeq_; }

private:
    struct PendingOutput {
        std::uint64_t seq;
        Reject result;
        std::size_t fillBegin;
        std::size_t fillCount;
    };

    void checkUsable() const {
        if (broken_) {
            throw std::runtime_error("engine unusable after a failed commit; reopen to recover");
        }
    }

    void open() {
        std::filesystem::create_directories(opts_.dir);
        for (const auto& entry : std::filesystem::directory_iterator(opts_.dir)) {
            std::string name = entry.path().filename().string();
            if (name.rfind("wal-", 0) == 0 || name.rfind("snap-", 0) == 0) {
                throw std::runtime_error("recovery not implemented");
            }
        }
        segment_ = 0;
        auto path = opts_.dir / walName(segment_);
        File file = File::create(path);
        writeWalHeader(file, segment_, 1);
        file.sync(opts_.sync);
        syncDir(opts_.dir, opts_.sync);
        wal_.emplace(std::move(file));
    }

    BookConfig cfg_;
    EngineOptions opts_;
    Book book_;

    std::uint64_t segment_ = 0;
    std::optional<File> wal_;

    std::vector<std::uint8_t> batch_;  // encoded records of the pending batch
    std::uint32_t pending_ = 0;
    Clock::time_point batchStart_{};

    std::vector<PendingOutput> outputs_;  // released ones first, then pending ones
    std::vector<Fill> fills_;
    std::vector<Fill> scratch_;
    std::size_t released_ = 0;

    std::uint64_t lastSeq_ = 0;
    std::uint64_t durableSeq_ = 0;
    bool broken_ = false;
};

}  // namespace harbor
