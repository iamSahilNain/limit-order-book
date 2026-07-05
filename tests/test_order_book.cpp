#include "lob/map_order_book.hpp"
#include "lob/array_order_book.hpp"
#include "bench/order_generator.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lob;

namespace {

std::vector<Trade> buf;

void add(IOrderBook& b, OrderId id, Side s, Price px, Quantity q,
         OrderType t = OrderType::Limit) {
    buf.clear();
    b.add(Order{id, s, t, px, q}, buf);
}

// ---------------------------------------------------------------------------
// Unit tests — each takes IOrderBook& so the same body runs against both books.
// ---------------------------------------------------------------------------

void test_rest_no_cross(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Buy,  100, 10);
    add(b, 2, Side::Sell, 101, 10);          // spread, no cross
    assert(b.best_bid().value() == 100);
    assert(b.best_ask().value() == 101);
    assert(buf.empty());
}

void test_full_cross(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Sell, 100, 10);
    add(b, 2, Side::Buy,  100, 10);          // exact cross
    assert(buf.size() == 1);
    assert(buf[0].qty == 10 && buf[0].price == 100 && buf[0].maker_id == 1);
    assert(!b.best_bid() && !b.best_ask());
}

void test_partial_fill_and_rest(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Sell, 100, 5);
    add(b, 2, Side::Buy,  100, 12);          // fills 5, rests 7 on the bid
    assert(buf.size() == 1 && buf[0].qty == 5);
    assert(b.best_bid().value() == 100);
    assert(!b.best_ask());
}

void test_fifo_priority(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Sell, 100, 5);           // earlier at 100
    add(b, 2, Side::Sell, 100, 5);           // later at 100
    add(b, 3, Side::Buy,  100, 5);           // must lift id 1 first
    assert(buf.size() == 1 && buf[0].maker_id == 1);
}

void test_price_priority_sweep(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Sell, 101, 5);
    add(b, 2, Side::Sell, 100, 5);           // better ask
    add(b, 3, Side::Buy,  101, 10);          // sweeps 100 then 101
    assert(buf.size() == 2);
    assert(buf[0].price == 100 && buf[1].price == 101);
    assert(!b.best_ask());
}

void test_cancel(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Buy, 100, 10);
    assert(b.cancel(1));
    assert(!b.cancel(1));                    // already gone
    assert(!b.best_bid());
}

void test_modify_qty_down_keeps_level(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Buy, 100, 10);
    buf.clear();
    assert(b.modify(1, 100, 4, buf));        // same price, qty down: in place
    assert(b.best_bid().value() == 100 && buf.empty());
}

void test_market_order(IOrderBook& b) {
    b.clear();
    add(b, 1, Side::Sell, 100, 3);
    add(b, 2, Side::Sell, 101, 3);
    add(b, 3, Side::Buy,  0,   4, OrderType::Market);  // price ignored; sweeps
    assert(buf.size() == 2 && buf[0].price == 100 && buf[1].price == 101);
    assert(b.best_ask().value() == 101);     // 2 left resting at 101
}

void run_suite(IOrderBook& b, const char* name) {
    test_rest_no_cross(b);
    test_full_cross(b);
    test_partial_fill_and_rest(b);
    test_fifo_priority(b);
    test_price_priority_sweep(b);
    test_cancel(b);
    test_modify_qty_down_keeps_level(b);
    test_market_order(b);
    std::printf("  %s: all 8 unit tests passed.\n", name);
}

// ---------------------------------------------------------------------------
// Differential / fuzz test: replay identical ops through both books and assert
// that every trade vector and best_bid/best_ask matches after each operation.
// ---------------------------------------------------------------------------
void test_differential() {
    // Generator params: mid=100000, tick=1, half_band=50 (sigma~16 ticks).
    // ArrayOrderBook band: [99000, 100999].  All prices are within this band.
    OrderGenerator gen(/*mid=*/100'000, /*tick=*/1, /*half_band_ticks=*/50,
                       /*cancel_ratio=*/0.25, /*seed=*/42);
    const auto ops = gen.generate(200'000);

    // Count add ops to size the pool exactly.
    std::size_t add_count = 0;
    for (const auto& op : ops)
        if (op.kind == GeneratedOp::Kind::Add) ++add_count;

    MapOrderBook   map_book;
    ArrayOrderBook arr_book(/*base=*/99'000, /*tick=*/1,
                            /*num_ticks=*/2'000, /*max_orders=*/add_count);

    std::vector<Trade> map_trades, arr_trades;
    map_trades.reserve(64);
    arr_trades.reserve(64);

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];
        map_trades.clear();
        arr_trades.clear();

        if (op.kind == GeneratedOp::Kind::Add) {
            map_book.add(op.order, map_trades);
            arr_book.add(op.order, arr_trades);
        } else {
            map_book.cancel(op.cancel_id);
            arr_book.cancel(op.cancel_id);
        }

        // Assert trade vectors are identical.
        assert(map_trades.size() == arr_trades.size());
        for (std::size_t t = 0; t < map_trades.size(); ++t) {
            assert(map_trades[t].taker_id == arr_trades[t].taker_id);
            assert(map_trades[t].maker_id == arr_trades[t].maker_id);
            assert(map_trades[t].price    == arr_trades[t].price);
            assert(map_trades[t].qty      == arr_trades[t].qty);
        }

        // Assert best bid/ask match.
        assert(map_book.best_bid() == arr_book.best_bid());
        assert(map_book.best_ask() == arr_book.best_ask());
    }

    std::printf("  Differential test (%zu ops): PASSED.\n", ops.size());
}

} // namespace

int main() {
    // Run the 8-test unit suite against both books.
    {
        MapOrderBook b;
        run_suite(b, "MapOrderBook");
    }
    {
        // Base 0, tick 1, 1000 slots covers prices [0,999]; tests use prices 100-101.
        ArrayOrderBook b(0, 1, 1000, 100'000);
        run_suite(b, "ArrayOrderBook");
    }

    test_differential();

    std::puts("All order book tests passed.");
    return 0;
}
