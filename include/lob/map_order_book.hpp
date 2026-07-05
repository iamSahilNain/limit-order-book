#pragma once
#include "lob/order_book.hpp"
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

namespace lob {

// Baseline order book: a std::map price ladder with a std::list FIFO per level and
// an id -> location index so cancel is an O(1) list splice.
//
// Complexity (L = distinct price levels on a side, F = resting orders consumed):
//   best_bid / best_ask : O(1)          -- map begin()
//   add (rests)         : O(log L)
//   add (matches)       : O(log L + F)
//   cancel              : O(log L)       -- splice is O(1); level erase is O(log L)
//
// This is the obvious design and the benchmark baseline. Checkpoint 1 swaps the
// map for a flat array to drop the log factor and improve cache locality; the
// benchmark then A/Bs the two -- that comparison is the headline resume claim.
class MapOrderBook final : public IOrderBook {
public:
    void add(const Order& order, std::vector<Trade>& out) override;
    bool cancel(OrderId id) override;
    bool modify(OrderId id, Price new_price, Quantity new_qty,
                std::vector<Trade>& out) override;
    std::optional<Price> best_bid() const override;
    std::optional<Price> best_ask() const override;
    void clear() override;

private:
    struct Resting { OrderId id; Quantity qty; };
    using Level = std::list<Resting>;   // FIFO; iterators stay valid across splices

    // Bids: highest first. Asks: lowest first. Best opposite price is begin().
    using BidLadder = std::map<Price, Level, std::greater<Price>>;
    using AskLadder = std::map<Price, Level, std::less<Price>>;

    struct Locator { Side side; Price price; Level::iterator it; };

    BidLadder bids_;
    AskLadder asks_;
    std::unordered_map<OrderId, Locator> index_;

    template <class Ladder>
    void match(const Order& taker, Ladder& opposite,
               std::vector<Trade>& out, Quantity& remaining);

    template <class Ladder>
    void rest(const Order& order, Ladder& same, Side side, Quantity remaining);
};

} // namespace lob
