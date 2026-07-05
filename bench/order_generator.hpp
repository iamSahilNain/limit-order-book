#pragma once
#include "lob/types.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace lob {

// Reproducible synthetic order flow. Prices cluster (normal) around a mid so the
// book stays populated and orders actually cross; a fraction of events cancel a
// previously added id. Deterministic given the seed -> repeatable benchmarks.
struct GeneratedOp {
    enum class Kind { Add, Cancel } kind;
    Order   order;      // valid when kind == Add
    OrderId cancel_id;  // valid when kind == Cancel
};

class OrderGenerator {
public:
    OrderGenerator(Price mid, Price tick, Price half_band_ticks,
                   double cancel_ratio = 0.25, std::uint64_t seed = 42)
        : mid_(mid), tick_(tick), band_(half_band_ticks),
          cancel_ratio_(cancel_ratio), rng_(seed) {}

    std::vector<GeneratedOp> generate(std::size_t n) {
        std::vector<GeneratedOp> ops;
        ops.reserve(n);

        std::vector<OrderId> live;                       // ids we could cancel
        std::uniform_real_distribution<double>  unit(0.0, 1.0);
        std::uniform_int_distribution<int>      side_d(0, 1);
        std::normal_distribution<double>        px_off(0.0, static_cast<double>(band_) / 3.0);
        std::uniform_int_distribution<Quantity> qty_d(1, 100);

        for (std::size_t i = 0; i < n; ++i) {
            if (!live.empty() && unit(rng_) < cancel_ratio_) {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                const std::size_t k = pick(rng_);
                ops.push_back({GeneratedOp::Kind::Cancel, {}, live[k]});
                live[k] = live.back();                   // swap-remove: O(1)
                live.pop_back();
            } else {
                const OrderId id = next_id_++;
                const Side side  = side_d(rng_) ? Side::Buy : Side::Sell;
                const int ticks  = static_cast<int>(std::llround(px_off(rng_)));
                const Price price = mid_ + static_cast<Price>(ticks) * tick_;
                ops.push_back({GeneratedOp::Kind::Add,
                               Order{id, side, OrderType::Limit, price, qty_d(rng_)}, 0});
                live.push_back(id);
            }
        }
        return ops;
    }

private:
    Price          mid_, tick_, band_;
    double         cancel_ratio_;
    std::mt19937_64 rng_;
    OrderId        next_id_ = 1;
};

} // namespace lob
