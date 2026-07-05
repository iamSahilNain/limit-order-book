#include "lob/map_order_book.hpp"
#include "lob/array_order_book.hpp"
#include "bench/order_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats { double throughput_ops_s, p50, p90, p99, p999, mean; };

Stats run(IOrderBook& book, const std::vector<GeneratedOp>& ops) {
    std::vector<Trade> trades;         // reused buffer -> no per-op allocation
    trades.reserve(64);
    std::vector<double> lat_ns;
    lat_ns.reserve(ops.size());

    const auto t0 = Clock::now();
    for (const auto& op : ops) {
        const auto s = Clock::now();
        if (op.kind == GeneratedOp::Kind::Add) {
            trades.clear();
            book.add(op.order, trades);
        } else {
            book.cancel(op.cancel_id);
        }
        const auto e = Clock::now();
        lat_ns.push_back(std::chrono::duration<double, std::nano>(e - s).count());
    }
    const auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) {
        return lat_ns[std::min(lat_ns.size() - 1,
                               static_cast<std::size_t>(p * lat_ns.size()))];
    };
    double sum = 0.0;
    for (double x : lat_ns) sum += x;

    return Stats{static_cast<double>(ops.size()) / secs,
                 pct(0.50), pct(0.90), pct(0.99), pct(0.999), sum / lat_ns.size()};
}

void report(const char* name, const Stats& s) {
    std::printf("%-16s %11.0f ops/s   p50 %6.0f  p90 %6.0f  p99 %7.0f  "
                "p99.9 %7.0f  mean %6.0f   (ns)\n",
                name, s.throughput_ops_s, s.p50, s.p90, s.p99, s.p999, s.mean);
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t N =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ULL;

    OrderGenerator gen(/*mid=*/100'000, /*tick=*/1, /*half_band_ticks=*/50);
    const auto ops = gen.generate(N);

    // Count add ops to size the ArrayOrderBook pool.
    std::size_t add_count = 0;
    for (const auto& op : ops)
        if (op.kind == GeneratedOp::Kind::Add) ++add_count;

    std::printf(
        "Replaying %zu ops.\n"
        "NOTE: per-op timing adds ~20-40 ns of std::chrono overhead per sample, so\n"
        "true op latency is a touch lower than shown. For sub-100 ns work prefer\n"
        "rdtsc or batch timing. Report throughput AND a latency percentile, and be\n"
        "ready to reconcile them (throughput ~= 1 / mean-latency).\n\n", N);

    Stats map_stats, arr_stats;

    {
        MapOrderBook book;
        // Warm caches/allocator, then reset before the measured run.
        { std::vector<Trade> t;
          for (const auto& op : ops)
              if (op.kind == GeneratedOp::Kind::Add) book.add(op.order, t);
          book.clear(); }
        map_stats = run(book, ops);
    }

    {
        // Band [99000, 100999] covers mid=100000 ± 1000 ticks; half_band=50 keeps
        // virtually all prices well inside the band (6-sigma < 100 ticks from mid).
        ArrayOrderBook book(/*base=*/99'000, /*tick=*/1,
                            /*num_ticks=*/2'000, /*max_orders=*/add_count);
        // Same warm-up pattern as MapOrderBook.
        { std::vector<Trade> t;
          for (const auto& op : ops)
              if (op.kind == GeneratedOp::Kind::Add) book.add(op.order, t);
          book.clear(); }
        arr_stats = run(book, ops);
    }

    report("MapOrderBook",   map_stats);
    report("ArrayOrderBook", arr_stats);

    return 0;
}
