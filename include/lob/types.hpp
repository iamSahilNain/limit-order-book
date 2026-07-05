#pragma once
#include <cstdint>

namespace lob {

// Prices are integers on a tick grid, NOT floats. Exact, comparable, cache-cheap,
// and the natural index for the flat-array ladder in Checkpoint 1. (Talking point:
// floats break exact price equality and priority — real venues use integer ticks.)
using OrderId  = std::uint64_t;
using Price    = std::int64_t;
using Quantity = std::uint64_t;

enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;   // ignored for Market orders
    Quantity  qty;
};

struct Trade {
    OrderId  taker_id;
    OrderId  maker_id;
    Price    price;    // trade executes at the resting (maker) price
    Quantity qty;
};

} // namespace lob
