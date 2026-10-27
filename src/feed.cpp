#include "harbor/feed.hpp"

#include <bit>

namespace harbor {

FeedEngine::FeedEngine(const BookConfig& cfg, const EngineOptions& options)
    : engine_(cfg, options), contexts_(std::bit_ceil(std::size_t{options.maxBatch} * 2)) {}

std::uint64_t FeedEngine::submit(const Command& cmd, Clock::time_point now, std::uint64_t origin) {
    OrderId id = targetOf(cmd);
    std::optional<RestingOrder> before;
    if (cmd.kind != Command::Kind::Add) {
        before = engine_.book().find(id);
    }
    // Record the context before submitting: submit may commit, but outputs are
    // only consumed by drain(), which runs after this returns.
    pushContext(CommandContext{cmd, before, 0, origin});
    std::size_t slot = (head_ + count_ - 1) & (contexts_.size() - 1);
    std::uint64_t seq = engine_.submit(cmd, now);
    auto after = engine_.book().find(id);
    contexts_[slot].restingAfter = after ? after->remaining : 0;
    return seq;
}

void FeedEngine::pushContext(const CommandContext& ctx) {
    if (count_ == contexts_.size()) {
        // The caller is not draining; grow (allocates, but never in steady state).
        std::vector<CommandContext> bigger(contexts_.size() * 2);
        for (std::size_t i = 0; i < count_; ++i) {
            bigger[i] = contexts_[(head_ + i) & (contexts_.size() - 1)];
        }
        contexts_ = std::move(bigger);
        head_ = 0;
    }
    contexts_[(head_ + count_) & (contexts_.size() - 1)] = ctx;
    ++count_;
}

bool FeedBook::apply(const wire::FeedEvent& e) {
    using Type = wire::FeedEvent::Type;
    if (e.seq != lastSeq_ + 1) {
        return false;
    }
    bool ok = false;
    switch (e.type) {
        case Type::Added:
            ok = book_.add(NewOrder{e.id, clientOf(e.id), e.side, e.price, e.qty, TimeInForce::GTC}, scratch_) ==
                     Reject::None &&
                 scratch_.empty();
            break;
        case Type::Executed: {
            auto o = book_.find(e.id);
            if (o && o->price == e.price && e.qty > 0 && e.qty <= o->remaining) {
                ok = e.qty == o->remaining ? book_.cancel(e.id) == Reject::None
                                           : book_.replace(e.id, o->price, o->remaining - e.qty, scratch_) ==
                                                 Reject::None;
            }
            break;
        }
        case Type::Deleted:
            ok = book_.cancel(e.id) == Reject::None;
            break;
        case Type::Reduced: {
            auto o = book_.find(e.id);
            ok = o && e.qty > 0 && e.qty <= o->remaining &&
                 book_.replace(e.id, o->price, e.qty, scratch_) == Reject::None;
            break;
        }
    }
    if (ok) {
        lastSeq_ = e.seq;
    }
    return ok;
}

bool FeedBook::loadSnapshot(std::uint64_t feedSeq, std::span<const wire::SnapshotOrderMsg> orders) {
    reset();
    for (const auto& o : orders) {
        if (book_.add(NewOrder{o.id, clientOf(o.id), o.side, o.price, o.qty, TimeInForce::GTC}, scratch_) !=
                Reject::None ||
            !scratch_.empty()) {
            reset();
            return false;
        }
    }
    lastSeq_ = feedSeq;
    return true;
}

void FeedBook::reset() {
    book_ = ReferenceBook(cfg_);
    lastSeq_ = 0;
}

}  // namespace harbor
