#include "harbor/fast_book.hpp"

#include <algorithm>
#include <bit>

namespace harbor {

namespace {

void setBit(std::vector<std::uint64_t>& bits, std::uint32_t i) {
    bits[i / 64] |= 1ULL << (i % 64);
}

void clearBit(std::vector<std::uint64_t>& bits, std::uint32_t i) {
    bits[i / 64] &= ~(1ULL << (i % 64));
}

bool testBit(const std::vector<std::uint64_t>& bits, std::uint32_t i) {
    return (bits[i / 64] >> (i % 64)) & 1ULL;
}

}  // namespace

FastBook::FastBook(const BookConfig& cfg)
    : cfg_(cfg),
      grid_(cfg),
      numLevels_(grid_.levels()),
      pool_(cfg.maxOrders),
      freeHead_(cfg.maxOrders == 0 ? kNone : 0),
      index_(cfg.maxOrders),
      bidLevels_(numLevels_),
      askLevels_(numLevels_),
      bidBits_((numLevels_ + 63) / 64, 0),
      askBits_((numLevels_ + 63) / 64, 0) {
    for (std::uint32_t i = 0; i < cfg.maxOrders; ++i) {
        pool_[i].next = i + 1 < cfg.maxOrders ? i + 1 : kNone;
    }
}

bool FastBook::crosses(Side side, std::uint32_t level) const {
    if (side == Side::Buy) {
        return bestAsk_ != kNone && level >= bestAsk_;
    }
    return bestBid_ != kNone && level <= bestBid_;
}

Reject FastBook::add(const NewOrder& order, std::vector<Fill>& out) {
    out.clear();
    if (order.qty == 0) {
        return Reject::BadQuantity;
    }
    if (!grid_.valid(order.price)) {
        return Reject::BadPrice;
    }
    if (index_.find(order.id) != nullptr) {
        return Reject::DuplicateId;
    }
    std::uint32_t level = grid_.levelOf(order.price);
    // A crossing order always frees a slot or fills completely, so only a
    // non-crossing GTC order can be refused for lack of space.
    if (count_ == cfg_.maxOrders && order.tif == TimeInForce::GTC &&
        !crosses(order.side, level)) {
        return Reject::BookFull;
    }
    execute(order.id, order.client, order.side, level, order.qty, order.tif, out);
    return Reject::None;
}

Reject FastBook::cancel(OrderId id) {
    const std::uint32_t* slot = index_.find(id);
    if (slot == nullptr) {
        return Reject::UnknownOrder;
    }
    removeOrder(*slot);
    return Reject::None;
}

Reject FastBook::replace(OrderId id, Price newPrice, Quantity newQty, std::vector<Fill>& out) {
    out.clear();
    const std::uint32_t* found = index_.find(id);
    if (found == nullptr) {
        return Reject::UnknownOrder;
    }
    if (newQty == 0) {
        return Reject::BadQuantity;
    }
    if (!grid_.valid(newPrice)) {
        return Reject::BadPrice;
    }
    std::uint32_t slot = *found;
    Order& o = pool_[slot];
    std::uint32_t newLevel = grid_.levelOf(newPrice);
    if (newLevel == o.level && newQty <= o.remaining) {
        Level& level = o.side == Side::Buy ? bidLevels_[o.level] : askLevels_[o.level];
        level.total -= o.remaining - newQty;
        o.remaining = newQty;  // keeps queue position
        return Reject::None;
    }
    Side side = o.side;
    ClientId client = o.client;
    removeOrder(slot);
    execute(id, client, side, newLevel, newQty, TimeInForce::GTC, out);
    return Reject::None;
}

void FastBook::execute(OrderId id, ClientId client, Side side, std::uint32_t level, Quantity qty,
                       TimeInForce tif, std::vector<Fill>& out) {
    std::vector<Level>& opposite = side == Side::Buy ? askLevels_ : bidLevels_;
    while (qty > 0 && crosses(side, level)) {
        std::uint32_t best = side == Side::Buy ? bestAsk_ : bestBid_;
        Level& lvl = opposite[best];
        Price tradePrice = grid_.priceOf(best);
        while (qty > 0 && lvl.head != kNone) {
            std::uint32_t slot = lvl.head;
            Order& passive = pool_[slot];
            Quantity fillQty = std::min(qty, passive.remaining);
            out.push_back(Fill{id, passive.id, tradePrice, fillQty});
            qty -= fillQty;
            if (fillQty == passive.remaining) {
                removeOrder(slot);  // may empty the level and move the cached best
            } else {
                passive.remaining -= fillQty;
                lvl.total -= fillQty;
            }
        }
    }
    if (qty > 0 && tif == TimeInForce::GTC) {
        rest(id, client, side, level, qty);
    }
}

void FastBook::rest(OrderId id, ClientId client, Side side, std::uint32_t level, Quantity qty) {
    std::uint32_t slot = freeHead_;
    freeHead_ = pool_[slot].next;

    Level& lvl = side == Side::Buy ? bidLevels_[level] : askLevels_[level];
    pool_[slot] = Order{id, client, qty, level, lvl.tail, kNone, side};
    if (lvl.tail == kNone) {
        lvl.head = slot;
    } else {
        pool_[lvl.tail].next = slot;
    }
    lvl.tail = slot;
    lvl.total += qty;

    if (side == Side::Buy) {
        setBit(bidBits_, level);
        if (bestBid_ == kNone || level > bestBid_) {
            bestBid_ = level;
        }
    } else {
        setBit(askBits_, level);
        if (bestAsk_ == kNone || level < bestAsk_) {
            bestAsk_ = level;
        }
    }
    index_.insert(id, slot);
    ++count_;
}

void FastBook::removeOrder(std::uint32_t slot) {
    Order& o = pool_[slot];
    Level& lvl = o.side == Side::Buy ? bidLevels_[o.level] : askLevels_[o.level];

    if (o.prev == kNone) {
        lvl.head = o.next;
    } else {
        pool_[o.prev].next = o.next;
    }
    if (o.next == kNone) {
        lvl.tail = o.prev;
    } else {
        pool_[o.next].prev = o.prev;
    }
    lvl.total -= o.remaining;

    index_.erase(o.id);
    --count_;
    Side side = o.side;
    std::uint32_t level = o.level;
    o.next = freeHead_;
    freeHead_ = slot;

    if (lvl.head == kNone) {
        onLevelEmptied(side, level);
    }
}

void FastBook::onLevelEmptied(Side side, std::uint32_t level) {
    if (side == Side::Buy) {
        clearBit(bidBits_, level);
        if (level == bestBid_) {
            bestBid_ = level == 0 ? kNone : highestBidAtOrBelow(level - 1);
        }
    } else {
        clearBit(askBits_, level);
        if (level == bestAsk_) {
            bestAsk_ = level + 1 >= numLevels_ ? kNone : lowestAskAtOrAbove(level + 1);
        }
    }
}

// Scans outward from the old best, so the common case (a nearby level is
// occupied) touches one or two words instead of the whole bitset.
std::uint32_t FastBook::highestBidAtOrBelow(std::uint32_t level) const {
    std::int64_t word = level / 64;
    std::uint32_t bit = level % 64;
    std::uint64_t mask = bit == 63 ? ~0ULL : (1ULL << (bit + 1)) - 1;
    for (; word >= 0; --word, mask = ~0ULL) {
        std::uint64_t w = bidBits_[static_cast<std::size_t>(word)] & mask;
        if (w != 0) {
            return static_cast<std::uint32_t>(word * 64 + 63 - std::countl_zero(w));
        }
    }
    return kNone;
}

std::uint32_t FastBook::lowestAskAtOrAbove(std::uint32_t level) const {
    std::size_t word = level / 64;
    std::uint64_t mask = ~0ULL << (level % 64);
    for (; word < askBits_.size(); ++word, mask = ~0ULL) {
        std::uint64_t w = askBits_[word] & mask;
        if (w != 0) {
            return static_cast<std::uint32_t>(word * 64 + std::countr_zero(w));
        }
    }
    return kNone;
}

std::optional<Price> FastBook::bestBid() const {
    if (bestBid_ == kNone) {
        return std::nullopt;
    }
    return grid_.priceOf(bestBid_);
}

std::optional<Price> FastBook::bestAsk() const {
    if (bestAsk_ == kNone) {
        return std::nullopt;
    }
    return grid_.priceOf(bestAsk_);
}

std::uint64_t FastBook::depthAt(Side side, Price price) const {
    if (!grid_.valid(price)) {
        return 0;
    }
    std::uint32_t level = grid_.levelOf(price);
    return side == Side::Buy ? bidLevels_[level].total : askLevels_[level].total;
}

bool FastBook::checkInvariants(std::string& error) const {
    if (bestBid_ != kNone && bestAsk_ != kNone && bestBid_ >= bestAsk_) {
        error = "book is crossed";
        return false;
    }
    std::size_t live = 0;
    auto checkSide = [&](const std::vector<Level>& levels, const std::vector<std::uint64_t>& bits,
                         Side side, std::uint32_t cachedBest) {
        std::uint32_t scannedBest = kNone;
        for (std::uint32_t i = 0; i < numLevels_; ++i) {
            const Level& lvl = levels[i];
            if ((lvl.head != kNone) != testBit(bits, i)) {
                error = "bitset mismatch at level " + std::to_string(i);
                return false;
            }
            std::uint64_t sum = 0;
            std::uint32_t prev = kNone;
            for (std::uint32_t s = lvl.head; s != kNone; prev = s, s = pool_[s].next) {
                const Order& o = pool_[s];
                const std::uint32_t* indexed = index_.find(o.id);
                if (o.prev != prev || o.level != i || o.side != side || o.remaining == 0 ||
                    indexed == nullptr || *indexed != s) {
                    error = "bad order in slot " + std::to_string(s);
                    return false;
                }
                sum += o.remaining;
                ++live;
            }
            if (prev != lvl.tail || sum != lvl.total) {
                error = "bad tail or total at level " + std::to_string(i);
                return false;
            }
            if (lvl.head != kNone && (scannedBest == kNone || side == Side::Buy)) {
                scannedBest = i;  // bids: last non-empty; asks: first non-empty
            }
        }
        if (scannedBest != cachedBest) {
            error = "cached best is stale";
            return false;
        }
        return true;
    };
    if (!checkSide(bidLevels_, bidBits_, Side::Buy, bestBid_) ||
        !checkSide(askLevels_, askBits_, Side::Sell, bestAsk_)) {
        return false;
    }
    if (live != count_ || index_.size() != count_) {
        error = "live orders " + std::to_string(live) + ", count " + std::to_string(count_) +
                ", index " + std::to_string(index_.size());
        return false;
    }
    return true;
}

}  // namespace harbor
