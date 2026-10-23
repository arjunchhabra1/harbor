#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <span>
#include <stdexcept>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/file_io.hpp"
#include "harbor/snapshot.hpp"
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
//
// Constructing an Engine on a directory that already holds data recovers it:
// the newest snapshot is restored and its log segment replayed. Destroying an
// Engine does not commit: pending commands are dropped, exactly as in a crash.
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
        // Room for typical sweeps even at tiny batch sizes, so steady state never grows it.
        fills_.reserve(std::max<std::size_t>(std::size_t{opts_.maxBatch} * 4, 4096));
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
        if (durableSeq_ - snapshotSeq_ >= opts_.snapshotEvery) {
            takeSnapshot();
        }
    }

    // Calls onOutput(const Output&) for each released output in seq order.
    template <class F>
    void drain(F&& onOutput) {
        if (released_ == 0) {
            return;  // callers drain after every submit; keep that O(1)
        }
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

    std::filesystem::path walPath(std::uint64_t segment) const { return opts_.dir / walName(segment); }
    std::filesystem::path snapPath(std::uint64_t segment) const { return opts_.dir / snapName(segment); }

    void hook(int step) {
        if (opts_.afterSnapshotStep) {
            opts_.afterSnapshotStep(step);
        }
    }

    // Creates a log segment holding only its header, durably.
    File createSegment(std::uint64_t segment, std::uint64_t firstSeq) {
        File file = File::create(walPath(segment));
        writeWalHeader(file, segment, firstSeq);
        file.sync(opts_.sync);
        syncDir(opts_.dir, opts_.sync);
        return file;
    }

    // Segment N -> N+1. A crash between any two steps leaves a recoverable
    // directory: the old snapshot and segment are deleted only after the new
    // ones are durable.
    void takeSnapshot() {
        try {
            std::uint64_t next = segment_ + 1;
            auto tmp = opts_.dir / (snapName(next) + ".tmp");
            writeSnapshotFile(tmp, captureSnapshot(book_, cfg_, next, durableSeq_), opts_.sync);
            hook(1);
            std::filesystem::rename(tmp, snapPath(next));
            syncDir(opts_.dir, opts_.sync);
            hook(2);
            wal_.emplace(createSegment(next, durableSeq_ + 1));
            std::uint64_t previous = segment_;
            segment_ = next;
            snapshotSeq_ = durableSeq_;
            hook(3);
            std::filesystem::remove(snapPath(previous));
            std::filesystem::remove(walPath(previous));
            syncDir(opts_.dir, opts_.sync);
            hook(4);
        } catch (...) {
            broken_ = true;
            throw;
        }
    }

    static std::optional<std::uint64_t> segmentOf(const std::string& name, const std::string& prefix,
                                                  const std::string& suffix) {
        if (name.size() != prefix.size() + 12 + suffix.size() || name.rfind(prefix, 0) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return std::nullopt;
        }
        std::uint64_t n = 0;
        for (char c : name.substr(prefix.size(), 12)) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return n;
    }

    void open() {
        std::filesystem::create_directories(opts_.dir);

        // 1. Leftover temp files are snapshots that were never renamed into place.
        std::optional<std::uint64_t> newestSnap;
        std::vector<std::filesystem::path> stale;
        std::vector<std::pair<std::uint64_t, std::filesystem::path>> segmentFiles;
        for (const auto& entry : std::filesystem::directory_iterator(opts_.dir)) {
            std::string name = entry.path().filename().string();
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                stale.push_back(entry.path());
            } else if (auto n = segmentOf(name, "snap-", ".bin")) {
                newestSnap = std::max(newestSnap.value_or(0), *n);
                segmentFiles.emplace_back(*n, entry.path());
            } else if (auto w = segmentOf(name, "wal-", ".log")) {
                segmentFiles.emplace_back(*w, entry.path());
            }
        }
        for (const auto& path : stale) {
            std::filesystem::remove(path);
        }

        // 2-3. Restore the newest snapshot, if any.
        segment_ = newestSnap.value_or(0);
        if (newestSnap) {
            Snapshot snap = readSnapshotFile(snapPath(segment_));
            if (snap.segment != segment_) {
                throw std::runtime_error("corrupt snapshot " + snapPath(segment_).string() +
                                         ": segment number does not match file name");
            }
            if (snap.cfg != cfg_) {
                throw std::invalid_argument("BookConfig differs from the one stored in " +
                                            snapPath(segment_).string());
            }
            restoreSnapshot(book_, snap);
            snapshotSeq_ = snap.lastSeq;
        }
        lastSeq_ = durableSeq_ = snapshotSeq_;

        // 4-6. Replay the segment, dropping a torn tail. A missing segment, or
        // one whose header never finished writing, holds nothing that was ever
        // acknowledged, so it is recreated.
        auto log = walPath(segment_);
        // Records are appended only after the header is synced, so a file no
        // longer than a header with an invalid header is a torn creation. A
        // bad header on a longer file is real corruption and scanWal throws.
        bool usable = std::filesystem::exists(log);
        if (usable && std::filesystem::file_size(log) <= kWalHeaderSize) {
            try {
                scanWal(log, [](std::uint64_t, const Command&) {});
            } catch (const std::runtime_error&) {
                usable = false;
            }
        }
        if (!usable) {
            std::filesystem::remove(log);
            wal_.emplace(createSegment(segment_, snapshotSeq_ + 1));
        } else {
            WalScan scan = scanWal(log, [&](std::uint64_t, const Command& cmd) {
                applyCommand(book_, cmd, scratch_);
            });
            if (scan.segment != segment_ || scan.firstSeq != snapshotSeq_ + 1) {
                throw std::runtime_error("corrupt log " + log.string() +
                                         ": header does not continue the snapshot");
            }
            if (scan.tornTail) {
                truncateFile(log, scan.validBytes);
            }
            lastSeq_ = durableSeq_ = scan.nextSeq - 1;
            wal_.emplace(File::openAppend(log));
            if (scan.tornTail) {
                wal_->sync(opts_.sync);
            }
        }
        scratch_.clear();

        // 7. Files from older segments are fully covered by the snapshot.
        bool removed = false;
        for (const auto& [n, path] : segmentFiles) {
            if (n < segment_) {
                std::filesystem::remove(path);
                removed = true;
            }
        }
        if (removed || !stale.empty()) {
            syncDir(opts_.dir, opts_.sync);
        }
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
    std::uint64_t snapshotSeq_ = 0;  // last seq covered by the current segment's snapshot
    bool broken_ = false;
};

}  // namespace harbor
