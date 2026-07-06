#include "lob/array_order_book.hpp"
#include <algorithm>

namespace lob {

ArrayOrderBook::ArrayOrderBook(Price base_price, Price tick_size,
                               std::size_t num_ticks, std::size_t max_orders)
    : base_price_(base_price), tick_size_(tick_size), num_ticks_(num_ticks),
      pool_(max_orders),
      levels_(num_ticks),
      best_bid_slot_(-1),
      best_ask_slot_(static_cast<long>(num_ticks)) {}

long ArrayOrderBook::price_to_slot(Price p) const noexcept {
    // Validate before dividing: integer division truncates toward zero, so a
    // price less than one tick below base would otherwise land on slot 0, and
    // an off-grid price (not a multiple of tick_size above base) would be
    // silently floored onto a neighboring level. Both must map to "invalid".
    const Price off = p - base_price_;
    if (off < 0 || off % tick_size_ != 0) return -1;
    const long s = static_cast<long>(off / tick_size_);
    if (static_cast<std::size_t>(s) >= num_ticks_) return -1;
    return s;
}

Price ArrayOrderBook::slot_to_price(long s) const noexcept {
    return base_price_ + static_cast<Price>(s) * tick_size_;
}

void ArrayOrderBook::push_back(std::uint32_t slot_u, std::uint32_t node_idx) {
    Level& lv    = levels_[slot_u];
    Node&  nd    = pool_[node_idx];
    nd.prev      = lv.tail;
    nd.next      = Node::NIL;
    if (lv.tail != Node::NIL) pool_[lv.tail].next = node_idx;
    else                      lv.head = node_idx;
    lv.tail = node_idx;
}

void ArrayOrderBook::unlink(std::uint32_t slot_u, std::uint32_t node_idx) {
    Level& lv = levels_[slot_u];
    Node&  nd = pool_[node_idx];
    if (nd.prev != Node::NIL) pool_[nd.prev].next = nd.next;
    else                      lv.head = nd.next;
    if (nd.next != Node::NIL) pool_[nd.next].prev = nd.prev;
    else                      lv.tail = nd.prev;
}

// Scan upward from `from` to find the next non-empty ask slot.
// Sets best_ask_slot_ to the found slot, or num_ticks_ (empty sentinel) if none.
void ArrayOrderBook::advance_ask(long from) {
    for (long s = from; s < static_cast<long>(num_ticks_); ++s) {
        if (levels_[static_cast<std::size_t>(s)].head != Node::NIL) {
            best_ask_slot_ = s;
            return;
        }
    }
    best_ask_slot_ = static_cast<long>(num_ticks_);
}

// Scan downward from `from` to find the next non-empty bid slot.
// Sets best_bid_slot_ to the found slot, or -1 (empty sentinel) if none.
void ArrayOrderBook::retreat_bid(long from) {
    for (long s = from; s >= 0; --s) {
        if (levels_[static_cast<std::size_t>(s)].head != Node::NIL) {
            best_bid_slot_ = s;
            return;
        }
    }
    best_bid_slot_ = -1;
}

// Buy taker vs. ask side: consume ascending ask levels from best_ask_slot_ upward.
void ArrayOrderBook::match_asks(const Order& taker, std::vector<Trade>& out,
                                Quantity& remaining) {
    while (remaining > 0 && best_ask_slot_ < static_cast<long>(num_ticks_)) {
        const Price opp_px = slot_to_price(best_ask_slot_);

        // Limit order: stop if best ask is above the taker's limit price.
        if (taker.type == OrderType::Limit && opp_px > taker.price) break;

        Level& lv = levels_[static_cast<std::size_t>(best_ask_slot_)];
        while (remaining > 0 && lv.head != Node::NIL) {
            Node& maker = pool_[lv.head];

            const Quantity   traded    = std::min(remaining, maker.qty);
            const OrderId    maker_id  = maker.id;
            const std::uint32_t nxt   = maker.next;

            out.push_back(Trade{taker.id, maker_id, opp_px, traded});
            remaining  -= traded;
            maker.qty  -= traded;

            if (maker.qty == 0) {
                id_to_node_.erase(maker_id);
                const std::uint32_t old_head = lv.head;
                lv.head = nxt;
                if (lv.head == Node::NIL) lv.tail = Node::NIL;
                else                      pool_[lv.head].prev = Node::NIL;
                pool_.deallocate(old_head);
            }
        }
        // If this level drained, advance the ask cursor to the next non-empty slot.
        if (lv.head == Node::NIL) advance_ask(best_ask_slot_ + 1);
        // If level did NOT drain (remaining reached 0 first), cursor stays; outer
        // loop exits on remaining == 0.
    }
}

// Sell taker vs. bid side: consume descending bid levels from best_bid_slot_ downward.
void ArrayOrderBook::match_bids(const Order& taker, std::vector<Trade>& out,
                                Quantity& remaining) {
    while (remaining > 0 && best_bid_slot_ >= 0) {
        const Price opp_px = slot_to_price(best_bid_slot_);

        // Limit order: stop if best bid is below the taker's limit price.
        if (taker.type == OrderType::Limit && opp_px < taker.price) break;

        Level& lv = levels_[static_cast<std::size_t>(best_bid_slot_)];
        while (remaining > 0 && lv.head != Node::NIL) {
            Node& maker = pool_[lv.head];

            const Quantity   traded    = std::min(remaining, maker.qty);
            const OrderId    maker_id  = maker.id;
            const std::uint32_t nxt   = maker.next;

            out.push_back(Trade{taker.id, maker_id, opp_px, traded});
            remaining  -= traded;
            maker.qty  -= traded;

            if (maker.qty == 0) {
                id_to_node_.erase(maker_id);
                const std::uint32_t old_head = lv.head;
                lv.head = nxt;
                if (lv.head == Node::NIL) lv.tail = Node::NIL;
                else                      pool_[lv.head].prev = Node::NIL;
                pool_.deallocate(old_head);
            }
        }
        // If this level drained, retreat the bid cursor to the next non-empty slot.
        if (lv.head == Node::NIL) retreat_bid(best_bid_slot_ - 1);
    }
}

void ArrayOrderBook::rest(const Order& order, long slot, Quantity remaining) {
    const std::uint32_t node_idx = pool_.allocate();
    // Arena exhausted: reject the residual rather than index arena_[NIL].
    // Same policy as an unrepresentable price -- the order does not rest.
    // (modify()'s re-add can never land here: its cancel just freed a node.)
    if (node_idx == Node::NIL) {
        ++rejected_;
        return;
    }
    Node& nd = pool_[node_idx];
    nd.id   = order.id;
    nd.qty  = remaining;
    nd.slot = static_cast<std::uint32_t>(slot);
    nd.side = order.side;

    push_back(static_cast<std::uint32_t>(slot), node_idx);
    id_to_node_[order.id] = node_idx;

    // Update cursor if this slot is a new best price on its side.
    if (order.side == Side::Buy) {
        if (slot > best_bid_slot_) best_bid_slot_ = slot;
    } else {
        if (slot < best_ask_slot_) best_ask_slot_ = slot;
    }
}

void ArrayOrderBook::add(const Order& order, std::vector<Trade>& out) {
    // Validate limit prices up front, the way a venue would: an order whose
    // price the ladder cannot represent (below base, off the tick grid, or
    // past the top of the band) is rejected whole -- it neither matches nor
    // rests. The old policy let such an order match first and then dropped
    // its residual, which left the book correct but the caller misled.
    // Market orders carry no price to validate.
    long slot = -1;
    if (order.type == OrderType::Limit) {
        slot = price_to_slot(order.price);
        if (slot < 0) {
            ++rejected_;
            return;
        }
    }

    Quantity remaining = order.qty;

    if (order.side == Side::Buy) {
        match_asks(order, out, remaining);
        if (remaining > 0 && order.type == OrderType::Limit)
            rest(order, slot, remaining);
    } else {
        match_bids(order, out, remaining);
        if (remaining > 0 && order.type == OrderType::Limit)
            rest(order, slot, remaining);
    }
}

bool ArrayOrderBook::cancel(OrderId id) {
    auto it = id_to_node_.find(id);
    if (it == id_to_node_.end()) return false;

    const std::uint32_t node_idx = it->second;
    id_to_node_.erase(it);

    const std::uint32_t slot_u = pool_[node_idx].slot;
    const Side          side   = pool_[node_idx].side;
    unlink(slot_u, node_idx);
    pool_.deallocate(node_idx);

    // Cursor maintenance: only scan when the cancelled order was AT the current best
    // AND its level is now empty.  Interior empty levels leave the cursors unchanged.
    if (levels_[slot_u].head == Node::NIL) {
        const long slot = static_cast<long>(slot_u);
        if (side == Side::Buy && slot == best_bid_slot_)
            retreat_bid(slot - 1);
        else if (side == Side::Sell && slot == best_ask_slot_)
            advance_ask(slot + 1);
    }

    return true;
}

bool ArrayOrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                            std::vector<Trade>& out) {
    auto it = id_to_node_.find(id);
    if (it == id_to_node_.end()) return false;

    // Refuse a target price the ladder cannot represent BEFORE touching the
    // resting order. Without this check the cancel+re-add path below would
    // cancel, fail to re-add, and report success -- the order would simply
    // vanish. Fail closed instead: return false, order untouched.
    if (price_to_slot(new_price) < 0) return false;

    Node& nd = pool_[it->second];
    const Price cur_price = slot_to_price(static_cast<long>(nd.slot));

    // Same price, quantity strictly down and non-zero: adjust in place (keep priority).
    if (new_price == cur_price && new_qty > 0 && new_qty <= nd.qty) {
        nd.qty = new_qty;
        return true;
    }

    // Otherwise cancel + re-add (loses priority; may cross and generate trades).
    const Side side = nd.side;
    cancel(id);
    add(Order{id, side, OrderType::Limit, new_price, new_qty}, out);
    return true;
}

std::optional<Price> ArrayOrderBook::best_bid() const {
    if (best_bid_slot_ < 0) return std::nullopt;
    return slot_to_price(best_bid_slot_);
}

std::optional<Price> ArrayOrderBook::best_ask() const {
    if (best_ask_slot_ >= static_cast<long>(num_ticks_)) return std::nullopt;
    return slot_to_price(best_ask_slot_);
}

void ArrayOrderBook::clear() {
    std::fill(levels_.begin(), levels_.end(), Level{});
    id_to_node_.clear();
    pool_.clear();
    best_bid_slot_ = -1;
    best_ask_slot_ = static_cast<long>(num_ticks_);
    rejected_ = 0;
}

} // namespace lob
