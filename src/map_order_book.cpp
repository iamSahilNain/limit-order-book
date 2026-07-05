#include "lob/map_order_book.hpp"

#include <algorithm>
#include <iterator>

namespace lob {

template <class Ladder>
void MapOrderBook::match(const Order& taker, Ladder& opposite,
                         std::vector<Trade>& out, Quantity& remaining) {
    while (remaining > 0 && !opposite.empty()) {
        auto lvl_it = opposite.begin();          // best opposite price
        const Price opp_px = lvl_it->first;

        if (taker.type == OrderType::Limit) {
            const bool crosses = (taker.side == Side::Buy) ? (opp_px <= taker.price)
                                                           : (opp_px >= taker.price);
            if (!crosses) break;                 // best price no longer marketable
        }

        Level& q = lvl_it->second;
        while (remaining > 0 && !q.empty()) {
            Resting& maker = q.front();          // FIFO: oldest at a level fills first
            const Quantity traded = std::min(remaining, maker.qty);

            out.push_back(Trade{taker.id, maker.id, opp_px, traded});
            remaining -= traded;
            maker.qty -= traded;

            if (maker.qty == 0) {
                index_.erase(maker.id);
                q.pop_front();
            }
        }
        if (q.empty()) opposite.erase(lvl_it);   // drop empty level
    }
}

template <class Ladder>
void MapOrderBook::rest(const Order& order, Ladder& same, Side side, Quantity remaining) {
    Level& q = same[order.price];
    q.push_back(Resting{order.id, remaining});
    index_[order.id] = Locator{side, order.price, std::prev(q.end())};
}

void MapOrderBook::add(const Order& order, std::vector<Trade>& out) {
    Quantity remaining = order.qty;

    if (order.side == Side::Buy) {
        match(order, asks_, out, remaining);
        if (remaining > 0 && order.type == OrderType::Limit)
            rest(order, bids_, Side::Buy, remaining);
    } else {
        match(order, bids_, out, remaining);
        if (remaining > 0 && order.type == OrderType::Limit)
            rest(order, asks_, Side::Sell, remaining);
    }
}

bool MapOrderBook::cancel(OrderId id) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return false;

    const Locator loc = idx_it->second;
    index_.erase(idx_it);

    if (loc.side == Side::Buy) {
        auto lvl_it = bids_.find(loc.price);
        lvl_it->second.erase(loc.it);            // O(1) splice out of the FIFO
        if (lvl_it->second.empty()) bids_.erase(lvl_it);
    } else {
        auto lvl_it = asks_.find(loc.price);
        lvl_it->second.erase(loc.it);
        if (lvl_it->second.empty()) asks_.erase(lvl_it);
    }
    return true;
}

bool MapOrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                          std::vector<Trade>& out) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return false;

    const Locator loc = idx_it->second;

    // Same price, quantity down (and non-zero): adjust in place, keep time priority.
    if (new_price == loc.price && new_qty > 0 && new_qty <= loc.it->qty) {
        loc.it->qty = new_qty;
        return true;
    }

    // Otherwise cancel + re-add. Loses priority; may cross and generate trades.
    const Side side = loc.side;
    cancel(id);
    add(Order{id, side, OrderType::Limit, new_price, new_qty}, out);
    return true;
}

std::optional<Price> MapOrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> MapOrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

void MapOrderBook::clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
}

} // namespace lob
