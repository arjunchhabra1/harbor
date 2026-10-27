#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "harbor/command.hpp"
#include "harbor/engine.hpp"
#include "harbor/fast_book.hpp"
#include "harbor/protocol.hpp"
#include "harbor/reference_book.hpp"
#include "harbor/types.hpp"

namespace harbor {

// Exchange order ids are (clientId << 32) | clientOrderId, so ownership needs no table.
inline ClientId clientOf(OrderId id) { return static_cast<ClientId>(id >> 32); }
inline std::uint32_t clientOrderIdOf(OrderId id) { return static_cast<std::uint32_t>(id); }
inline OrderId exchangeId(ClientId client, std::uint32_t clientOrderId) {
    return (OrderId{client} << 32) | clientOrderId;
}

inline OrderId targetOf(const Command& cmd) {
    return cmd.kind == Command::Kind::Add ? cmd.order.id : cmd.id;
}

// What the generator needs to know about a command beyond its output.
struct CommandContext {
    Command cmd;
    std::optional<RestingOrder> before;  // the target order before the command (cancel/replace)
    Quantity restingAfter;               // the target order's quantity in the book right after it
    std::uint64_t origin;                // opaque tag passed back with the response (gateway session)
};

// Turns released engine outputs into feed events, client responses, and
// execution reports. Assigns feed seqs and match ids; both start at 1.
class FeedGenerator {
public:
    // emitEvent(const wire::FeedEvent&), emitResponse(const wire::ResponseMsg&, ClientId, origin),
    // emitExecution(const wire::ExecutionMsg&, ClientId owner).
    template <class Events, class Responses, class Executions>
    void onOutput(const CommandContext& ctx, const Output& out, Events&& emitEvent, Responses&& emitResponse,
                  Executions&& emitExecution) {
        using wire::FeedEvent;
        const Command& cmd = ctx.cmd;
        OrderId id = targetOf(cmd);
        ClientId client = clientOf(id);
        std::uint8_t command = cmd.kind == Command::Kind::Add      ? 2
                               : cmd.kind == Command::Kind::Cancel ? 3
                                                                   : 4;
        emitResponse(wire::ResponseMsg{clientOrderIdOf(id), command, out.result, out.seq, ctx.restingAfter}, client,
                     ctx.origin);
        if (out.result != Reject::None) {
            return;  // rejected commands change nothing, so the feed says nothing
        }

        auto fillsThenRest = [&](Side side, Price price, Quantity qty, bool mayRest) {
            Quantity filled = 0;
            for (const Fill& f : out.fills) {
                std::uint64_t match = nextMatch_++;
                emitEvent(FeedEvent{FeedEvent::Type::Executed, ++feedSeq_, f.resting, Side::Buy, f.price, f.qty, match});
                emitExecution(wire::ExecutionMsg{clientOrderIdOf(id), out.seq, f.price, f.qty,
                                                 wire::ExecutionMsg::Aggressor, match},
                              client);
                emitExecution(wire::ExecutionMsg{clientOrderIdOf(f.resting), out.seq, f.price, f.qty,
                                                 wire::ExecutionMsg::Resting, match},
                              clientOf(f.resting));
                filled += f.qty;
            }
            if (mayRest && qty > filled) {
                emitEvent(FeedEvent{FeedEvent::Type::Added, ++feedSeq_, id, side, price, qty - filled, 0});
            }
        };

        switch (cmd.kind) {
            case Command::Kind::Add:
                fillsThenRest(cmd.order.side, cmd.order.price, cmd.order.qty, cmd.order.tif == TimeInForce::GTC);
                break;
            case Command::Kind::Cancel:
                emitEvent(FeedEvent{FeedEvent::Type::Deleted, ++feedSeq_, id, Side::Buy, 0, 0, 0});
                break;
            case Command::Kind::Replace: {
                const RestingOrder& before = *ctx.before;  // an accepted replace had a resting target
                if (cmd.price == before.price && cmd.qty <= before.remaining) {
                    if (cmd.qty != before.remaining) {
                        emitEvent(FeedEvent{FeedEvent::Type::Reduced, ++feedSeq_, id, Side::Buy, 0, cmd.qty, 0});
                    }
                } else {
                    emitEvent(FeedEvent{FeedEvent::Type::Deleted, ++feedSeq_, id, Side::Buy, 0, 0, 0});
                    fillsThenRest(before.side, cmd.price, cmd.qty, true);
                }
                break;
            }
        }
    }

    std::uint64_t lastFeedSeq() const { return feedSeq_; }

private:
    std::uint64_t feedSeq_ = 0;
    std::uint64_t nextMatch_ = 1;
};

// The v2 engine plus a FeedGenerator: remembers each command's context until
// its output is released, then turns released outputs into feed events,
// responses, and executions. Everything it emits is durable.
class FeedEngine {
public:
    using Clock = Engine<FastBook>::Clock;

    FeedEngine(const BookConfig& cfg, const EngineOptions& options);

    // origin is returned with the command's response (the gateway passes its session token).
    std::uint64_t submit(const Command& cmd, Clock::time_point now, std::uint64_t origin = 0);
    void poll(Clock::time_point now) { engine_.poll(now); }
    void commit() { engine_.commit(); }

    template <class Events, class Responses, class Executions>
    void drain(Events&& emitEvent, Responses&& emitResponse, Executions&& emitExecution) {
        engine_.drain([&](const Output& out) {
            gen_.onOutput(contexts_[head_], out, emitEvent, emitResponse, emitExecution);
            head_ = (head_ + 1) & (contexts_.size() - 1);
            --count_;
        });
    }

    const FastBook& book() const { return engine_.book(); }
    std::uint64_t lastSeq() const { return engine_.lastSeq(); }
    std::uint64_t lastDurableSeq() const { return engine_.lastDurableSeq(); }
    std::uint64_t pendingCount() const { return engine_.lastSeq() - engine_.lastDurableSeq(); }
    std::uint64_t lastFeedSeq() const { return gen_.lastFeedSeq(); }

private:
    void pushContext(const CommandContext& ctx);

    Engine<FastBook> engine_;
    FeedGenerator gen_;
    std::vector<CommandContext> contexts_;  // circular FIFO, power-of-two size; grows only if never drained
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

// Rebuilds a book from feed events alone (what a market-data listener does).
class FeedBook {
public:
    explicit FeedBook(const BookConfig& cfg) : cfg_(cfg), book_(cfg) {}

    // False if the event is out of sequence or inconsistent with the book.
    bool apply(const wire::FeedEvent& event);
    // Replaces the book with a snapshot taken at feedSeq.
    bool loadSnapshot(std::uint64_t feedSeq, std::span<const wire::SnapshotOrderMsg> orders);
    void reset();

    std::uint64_t lastSeq() const { return lastSeq_; }
    const ReferenceBook& book() const { return book_; }

private:
    BookConfig cfg_;
    ReferenceBook book_;
    std::uint64_t lastSeq_ = 0;
    std::vector<Fill> scratch_;
};

}  // namespace harbor
