#pragma once
#include "lob/types.hpp"
#include <optional>
#include <vector>

namespace lob {

// Abstract order book. Two implementations are benchmarked against it:
//   MapOrderBook   - std::map price ladder            (baseline, provided)
//   ArrayOrderBook - flat-array price ladder          (Checkpoint 1, yours)
//
// Design choices worth defending in an interview:
//   * add() appends trades to a caller-owned buffer that is reused across calls,
//     so the hot path does no per-order heap allocation once warm.
//   * The interface is virtual purely for pluggability and A/B benchmarking. A
//     production engine would template/CRTP the book to remove vtable indirection
//     from the hot path -- know why, and be ready to say so.
class IOrderBook {
public:
    virtual ~IOrderBook() = default;

    // Submit a new order. Matches the opposite side by price-time (FIFO) priority.
    // Unfilled remainder of a Limit order rests; unfilled Market qty is discarded.
    // Generated trades are appended to `out` (caller clears/reuses it).
    // Precondition: `order.id` is unique among live (resting) orders.
    virtual void add(const Order& order, std::vector<Trade>& out) = 0;

    // Remove a resting order. Returns false for an unknown id.
    virtual bool cancel(OrderId id) = 0;

    // A pure quantity decrease at the same price keeps time priority; any other
    // change is cancel + re-add (loses priority, may cross and match).
    // new_qty == 0 degenerates to a cancel: the re-add rests nothing.
    virtual bool modify(OrderId id, Price new_price, Quantity new_qty,
                        std::vector<Trade>& out) = 0;

    virtual std::optional<Price> best_bid() const = 0;
    virtual std::optional<Price> best_ask() const = 0;

    virtual void clear() = 0;
};

} // namespace lob
