#pragma once

#include <cstdint>
#include <random>
#include <vector>
#include "harbor/types.hpp"

namespace harbor::testing {

struct Op {
    enum class Kind { Add, Cancel, Replace } kind;
    NewOrder order;  // Add
    OrderId id;      // Cancel / Replace
    Price price;     // Replace
    Quantity qty;    // Replace
};

// Seeded stream of book operations: 60% add (20% of them IOC), 30% cancel,
// 10% replace. Prices cluster around `mid` with the sides overlapping, and a small
// share of ids, prices, and quantities are invalid to exercise every reject.
//
// Cancel and replace targets are drawn from orders that are actually resting,
// using the `isLive` predicate (pass a book's `contains`). Without it, almost
// every cancel would miss, and old orders would pile up until the book is full.
class OpGenerator {
public:
    OpGenerator(std::uint64_t seed, Price mid, Price halfWindow, Price minPrice, Price maxPrice)
        : rng_(seed), mid_(mid), half_(halfWindow), minPrice_(minPrice), maxPrice_(maxPrice) {}

    template <class IsLive>
    Op next(IsLive&& isLive) {
        Op op{};
        std::uint64_t roll = percent();
        if (roll < 60) {
            op.kind = Op::Kind::Add;
            op.order.side = rng_() % 2 ? Side::Buy : Side::Sell;
            op.order.price = price(op.order.side);
            op.order.client = static_cast<ClientId>(1 + rng_() % 4);
            op.order.qty = qty();
            op.order.tif = percent() < 20 ? TimeInForce::IOC : TimeInForce::GTC;
            Live* live = percent() < 2 ? pickLive(isLive) : nullptr;
            if (live != nullptr) {
                op.order.id = live->id;  // DuplicateId
            } else {
                op.order.id = nextId_++;
                if (op.order.tif == TimeInForce::GTC) {
                    live_.push_back(Live{op.order.id, op.order.price});
                }
            }
            return op;
        }
        op.kind = roll < 90 ? Op::Kind::Cancel : Op::Kind::Replace;
        op.price = price(rng_() % 2 ? Side::Buy : Side::Sell);
        op.qty = qty();
        Live* live = percent() < 90 ? pickLive(isLive) : nullptr;
        if (live == nullptr) {
            op.id = (1ULL << 62) + rng_() % 1'000'000;  // never issued: UnknownOrder
            return op;
        }
        op.id = live->id;
        if (op.kind == Op::Kind::Replace) {
            // Same-price replaces exercise the keep/lose queue-priority rule.
            if (percent() < 30) {
                op.price = live->price;
            } else if (op.price >= minPrice_ && op.price <= maxPrice_) {
                live->price = op.price;
            }
        }
        return op;
    }

private:
    struct Live {
        OrderId id;
        Price price;
    };

    std::uint64_t percent() { return rng_() % 100; }

    // Buys land mostly below mid and sells mostly above, overlapping by
    // kOverlap ticks, so the book builds depth on both sides and a steady share
    // of orders cross the spread.
    Price price(Side side) {
        if (percent() < 1) {
            return rng_() % 2 ? minPrice_ - 1 : maxPrice_ + 1;
        }
        constexpr Price kOverlap = 10;
        Price offset = static_cast<Price>(rng_() % static_cast<std::uint64_t>(half_ + kOverlap + 1));
        return side == Side::Buy ? mid_ + kOverlap - offset : mid_ - kOverlap + offset;
    }

    Quantity qty() {
        if (percent() < 1) {
            return 0;
        }
        return static_cast<Quantity>(1 + rng_() % 100);
    }

    // Random resting order, pruning entries that have since filled or been
    // cancelled. Returns nullptr when nothing is resting.
    template <class IsLive>
    Live* pickLive(IsLive& isLive) {
        while (!live_.empty()) {
            std::size_t i = rng_() % live_.size();
            if (isLive(live_[i].id)) {
                return &live_[i];
            }
            live_[i] = live_.back();
            live_.pop_back();
        }
        return nullptr;
    }

    std::mt19937_64 rng_;
    Price mid_, half_, minPrice_, maxPrice_;
    OrderId nextId_ = 1;
    std::vector<Live> live_;  // GTC orders that may still be resting
};

}  // namespace harbor::testing
