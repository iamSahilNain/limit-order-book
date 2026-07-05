#pragma once
#include "lob/order_book.hpp"
#include "lob/memory_pool.hpp"
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace lob {

// Cache-friendly flat-array price ladder with pool-allocated intrusive FIFOs.
//
// Slot mapping:  slot = (price - base_price) / tick_size
// Band:          slots [0, num_ticks)  ->  prices [base_price, base_price + num_ticks*tick_size)
//
// Out-of-band policy: a resting limit order whose price maps to a slot outside
// [0, num_ticks) is silently rejected — no trades are produced and no order rests.
// The benchmark and test configurations are sized so this never triggers.
//
// Complexity:
//   best_bid / best_ask : O(1)   -- direct cursor read
//   add (rests)         : O(1)   -- pool alloc + FIFO append + cursor update
//   add (matches)       : O(F)   -- F = resting orders consumed, plus bounded cursor scan
//   cancel              : O(1)   -- O(1) intrusive unlink + bounded cursor scan if level empties
class ArrayOrderBook final : public IOrderBook {
public:
    // base_price  : lowest representable price (slot 0)
    // tick_size   : price increment
    // num_ticks   : number of price slots in the ladder
    // max_orders  : node pool capacity (preallocated; must cover peak live orders)
    ArrayOrderBook(Price base_price, Price tick_size,
                   std::size_t num_ticks, std::size_t max_orders);

    void add(const Order& order, std::vector<Trade>& out) override;
    bool cancel(OrderId id) override;
    bool modify(OrderId id, Price new_price, Quantity new_qty,
                std::vector<Trade>& out) override;
    std::optional<Price> best_bid() const override;
    std::optional<Price> best_ask() const override;
    void clear() override;

private:
    // Intrusive doubly linked FIFO per price slot.
    // head = oldest (next to fill); tail = newest (last inserted).
    // Empty iff head == Node::NIL.
    struct Level { std::uint32_t head = Node::NIL, tail = Node::NIL; };

    Price       base_price_;
    Price       tick_size_;
    std::size_t num_ticks_;

    NodePool                                   pool_;
    std::vector<Level>                         levels_;       // size == num_ticks_
    std::unordered_map<OrderId, std::uint32_t> id_to_node_;  // id -> pool index

    // Cursors:
    //   best_bid_slot_ : highest occupied bid slot; sentinel = -1 (no bids)
    //   best_ask_slot_ : lowest  occupied ask slot; sentinel = (long)num_ticks_ (no asks)
    long best_bid_slot_;
    long best_ask_slot_;

    // Price <-> slot helpers. price_to_slot returns -1 for out-of-band prices.
    long  price_to_slot(Price p) const noexcept;
    Price slot_to_price(long s)  const noexcept;

    // O(1) FIFO append and arbitrary-position unlink (via prev/next).
    void push_back(std::uint32_t slot_u, std::uint32_t node_idx);
    void unlink   (std::uint32_t slot_u, std::uint32_t node_idx);

    // Cursor maintenance after a level empties:
    //   advance_ask: scan upward   from `from` for next non-empty ask slot.
    //   retreat_bid: scan downward from `from` for next non-empty bid slot.
    // Both set the empty sentinel if no non-empty slot is found.
    void advance_ask(long from);
    void retreat_bid(long from);

    // Matching loops (mirror MapOrderBook::match exactly).
    void match_asks(const Order& taker, std::vector<Trade>& out, Quantity& remaining);
    void match_bids(const Order& taker, std::vector<Trade>& out, Quantity& remaining);

    // Allocate a node and append it to the slot's FIFO; update cursors.
    void rest(const Order& order, long slot, Quantity remaining);
};

} // namespace lob
