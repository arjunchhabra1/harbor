#include "harbor/reference_book.hpp"

#include <algorithm>

namespace harbor {

ReferenceBook::ReferenceBook(const BookConfig& cfg) : cfg_(cfg) {}

bool ReferenceBook::validPrice(Price price) const {
    return price >= cfg_.minPrice && price <= cfg_.maxPrice &&
           (price - cfg_.minPrice) % cfg_.tick == 0;
}

bool ReferenceBook::crosses(Side side, Price price) const {
    if (side == Side::Buy) {
        return !asks_.empty() && price >= asks_.begin()->first;
    }
    return !bids_.empty() && price <= bids_.begin()->first;
}

Reject ReferenceBook::add(const NewOrder& order, std::vector<Fill>& out) {
    out.clear();
    if (order.qty == 0) {
        return Reject::BadQuantity;
    }
    if (!validPrice(order.price)) {
        return Reject::BadPrice;
    }
    if (index_.contains(order.id)) {
        return Reject::DuplicateId;
    }
    // A crossing order always frees a slot or fills completely, so only a
    // non-crossing GTC order can be refused for lack of space.
    if (index_.size() == cfg_.maxOrders && order.tif == TimeInForce::GTC &&
        !crosses(order.side, order.price)) {
        return Reject::BookFull;
    }
    execute(order.id, order.client, order.side, order.price, order.qty, order.tif, out);
    return Reject::None;
}

Reject ReferenceBook::cancel(OrderId id) {
    if (!index_.contains(id)) {
        return Reject::UnknownOrder;
    }
    remove(id);
    return Reject::None;
}

Reject ReferenceBook::replace(OrderId id, Price newPrice, Quantity newQty,
                              std::vector<Fill>& out) {
    out.clear();
    auto found = index_.find(id);
    if (found == index_.end()) {
        return Reject::UnknownOrder;
    }
    if (newQty == 0) {
        return Reject::BadQuantity;
    }
    if (!validPrice(newPrice)) {
        return Reject::BadPrice;
    }
    Locator loc = found->second;
    if (newPrice == loc.price && newQty <= loc.it->remaining) {
        loc.it->remaining = newQty;  // keeps queue position
        return Reject::None;
    }
    ClientId client = loc.it->client;
    remove(id);
    execute(id, client, loc.side, newPrice, newQty, TimeInForce::GTC, out);
    return Reject::None;
}

void ReferenceBook::execute(OrderId id, ClientId client, Side side, Price price, Quantity qty,
                            TimeInForce tif, std::vector<Fill>& out) {
    auto matchAgainst = [&](auto& levels) {
        while (qty > 0 && !levels.empty() && crosses(side, price)) {
            auto level = levels.begin();
            Queue& queue = level->second;
            while (qty > 0 && !queue.empty()) {
                Resting& passive = queue.front();
                Quantity fillQty = std::min(qty, passive.remaining);
                out.push_back(Fill{id, passive.id, level->first, fillQty});
                qty -= fillQty;
                passive.remaining -= fillQty;
                if (passive.remaining == 0) {
                    index_.erase(passive.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) {
                levels.erase(level);
            }
        }
    };
    if (side == Side::Buy) {
        matchAgainst(asks_);
    } else {
        matchAgainst(bids_);
    }
    if (qty > 0 && tif == TimeInForce::GTC) {
        rest(id, client, side, price, qty);
    }
}

void ReferenceBook::rest(OrderId id, ClientId client, Side side, Price price, Quantity qty) {
    Queue& queue = side == Side::Buy ? bids_[price] : asks_[price];
    queue.push_back(Resting{id, client, qty});
    index_.emplace(id, Locator{side, price, std::prev(queue.end())});
}

void ReferenceBook::remove(OrderId id) {
    auto found = index_.find(id);
    Locator loc = found->second;
    index_.erase(found);
    auto eraseFrom = [&](auto& levels) {
        auto level = levels.find(loc.price);
        level->second.erase(loc.it);
        if (level->second.empty()) {
            levels.erase(level);
        }
    };
    if (loc.side == Side::Buy) {
        eraseFrom(bids_);
    } else {
        eraseFrom(asks_);
    }
}

std::optional<Price> ReferenceBook::bestBid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> ReferenceBook::bestAsk() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::uint64_t ReferenceBook::depthAt(Side side, Price price) const {
    auto sum = [&](const auto& levels) {
        std::uint64_t total = 0;
        auto level = levels.find(price);
        if (level != levels.end()) {
            for (const Resting& o : level->second) {
                total += o.remaining;
            }
        }
        return total;
    };
    return side == Side::Buy ? sum(bids_) : sum(asks_);
}

bool ReferenceBook::checkInvariants(std::string& error) const {
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        error = "book is crossed";
        return false;
    }
    std::size_t live = 0;
    auto checkSide = [&](const auto& levels, Side side) {
        for (const auto& [price, queue] : levels) {
            if (queue.empty()) {
                error = "empty level at " + std::to_string(price);
                return false;
            }
            for (const Resting& o : queue) {
                ++live;
                auto found = index_.find(o.id);
                if (o.remaining == 0 || found == index_.end() || found->second.side != side ||
                    found->second.price != price) {
                    error = "bad resting order " + std::to_string(o.id);
                    return false;
                }
            }
        }
        return true;
    };
    if (!checkSide(bids_, Side::Buy) || !checkSide(asks_, Side::Sell)) {
        return false;
    }
    if (live != index_.size() || live > cfg_.maxOrders) {
        error = "index size " + std::to_string(index_.size()) + " != live orders " +
                std::to_string(live);
        return false;
    }
    return true;
}

}  // namespace harbor
