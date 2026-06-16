// Microbenchmark: market-data-in -> order-ready latency on the strategy hot path.
// Measures internal compute latency only. Excludes rate limiter and network I/O.
//
// Three scenarios:
//   1. SimpleMomentum, trades that never cross threshold (no order generated)
//   2. SimpleMomentum, trades that always cross threshold (order on every tick)
//   3. AlwaysOrder, places a limit order on every quote (pure submission path)
//
// Reports per-call nanoseconds: mean, p50, p90, p99, p99.9, max.

#include "strategy/example_strategies.hpp"
#include "strategy/strategy_base.hpp"
#include "types/trade.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <x86intrin.h>
#define BENCH_HAS_RDTSC 1
#endif

using namespace hft;

namespace {

// Compiler barrier: prevent reorder/DCE around timed sections.
#if defined(__GNUC__) || defined(__clang__)
#define BENCH_BARRIER() asm volatile("" ::: "memory")
#else
#define BENCH_BARRIER() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif

// rdtsc with lfence before to prevent earlier instructions from leaking past the read.
// Plain __rdtsc on its own is enough for batch timing; lfence tightens per-call.
inline uint64_t rdtsc_start() {
#if BENCH_HAS_RDTSC
    _mm_lfence();
    return __rdtsc();
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

inline uint64_t rdtsc_end() {
#if BENCH_HAS_RDTSC
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

double calibrate_ns_per_tick() {
#if BENCH_HAS_RDTSC
    // Calibrate against steady_clock over ~250ms.
    using clk = std::chrono::steady_clock;
    const uint64_t c0 = rdtsc_start();
    const auto t0 = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto t1 = clk::now();
    const uint64_t c1 = rdtsc_end();
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double ticks = static_cast<double>(c1 - c0);
    return ns / ticks;
#else
    return 1.0;
#endif
}

class NoopSink final : public IExecutionSink {
public:
    uint64_t submitted{0};
    volatile double black_hole{0.0};  // defeat DCE on order fields

    bool submit_trade(const Trade& trade,
                      OrderCallback /*response_cb*/,
                      StorageFailureCallback /*storage_fail_cb*/) override {
        black_hole = trade.price + trade.quantity;
        ++submitted;
        return true;
    }
    void cancel_order(const std::string&, uint64_t, uint64_t) override {}
    void cancel_all_orders(const std::string&) override {}
};

// Strategy that places a limit order on every quote. Measures pure submission path
// without the conditional branches inside SimpleMomentum / SimpleMarketMaker.
class AlwaysQuoteOrderStrategy final : public StrategyBase {
public:
    AlwaysQuoteOrderStrategy(uint32_t id, const std::string& ticker)
        : StrategyBase(id, "AlwaysOrder_" + ticker, {ticker}) {}

    void on_quote(const std::string& ticker,
                  double bid,
                  double ask,
                  double /*bid_size*/,
                  double /*ask_size*/) override {
        if (!is_active()) return;
        const double mid = (bid + ask) * 0.5;
        place_limit_order(ticker.c_str(), OrderSide::BUY, 1.0, mid - 0.5);
    }

    void on_trade(const std::string&, double, double, bool) override {}
};

int64_t estimate_timer_overhead_ticks(size_t iters) {
    std::vector<int64_t> deltas(iters);
    for (size_t i = 0; i < iters; ++i) {
        BENCH_BARRIER();
        const uint64_t a = rdtsc_start();
        const uint64_t b = rdtsc_end();
        BENCH_BARRIER();
        deltas[i] = static_cast<int64_t>(b - a);
    }
    std::sort(deltas.begin(), deltas.end());
    return deltas[iters / 2];
}

struct Stats {
    double mean_ns;
    double p50_ns, p90_ns, p99_ns, p999_ns, max_ns;
};

Stats compute_stats(std::vector<int64_t>& ticks, double ns_per_tick, int64_t overhead_ticks) {
    std::sort(ticks.begin(), ticks.end());
    const size_t n = ticks.size();
    auto to_ns = [&](int64_t t) {
        double v = static_cast<double>(t - overhead_ticks) * ns_per_tick;
        return v < 0.0 ? 0.0 : v;
    };
    Stats s{};
    s.p50_ns  = to_ns(ticks[n * 50 / 100]);
    s.p90_ns  = to_ns(ticks[n * 90 / 100]);
    s.p99_ns  = to_ns(ticks[n * 99 / 100]);
    s.p999_ns = to_ns(ticks[(n * 999) / 1000]);
    s.max_ns  = to_ns(ticks.back());
    double sum_ticks = 0.0;
    for (auto v : ticks) sum_ticks += static_cast<double>(v);
    const double mean_ticks = sum_ticks / static_cast<double>(n);
    s.mean_ns = (mean_ticks - static_cast<double>(overhead_ticks)) * ns_per_tick;
    if (s.mean_ns < 0.0) s.mean_ns = 0.0;
    return s;
}

void print_header() {
    std::printf("%-46s | %9s %8s %8s %9s %9s %12s %12s\n",
                "Scenario", "mean(ns)", "p50", "p90", "p99", "p99.9", "max(ns)", "orders");
    std::printf("---------------------------------------------- "
                "| --------- -------- -------- --------- --------- ------------ ------------\n");
}

void print_stats(const char* label, const Stats& s, uint64_t orders) {
    std::printf("%-46s | %9.1f %8.1f %8.1f %9.1f %9.1f %12.1f %12llu\n",
                label,
                s.mean_ns,
                s.p50_ns,
                s.p90_ns,
                s.p99_ns,
                s.p999_ns,
                s.max_ns,
                (unsigned long long)orders);
}

void copy_ticker(const char* src, char* dst, size_t n) {
    std::strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

// Trades with tiny noise — well below 1% momentum threshold, so no orders placed.
std::vector<TradeEvent> gen_trades_cold(size_t n, const char* ticker) {
    std::vector<TradeEvent> v(n);
    std::mt19937_64 rng(1234567);
    std::uniform_real_distribution<double> noise(-0.0005, 0.0005);  // ±0.05%
    const double base = 50000.0;
    for (size_t i = 0; i < n; ++i) {
        copy_ticker(ticker, v[i].ticker, sizeof(v[i].ticker));
        v[i].price = base * (1.0 + noise(rng));
        v[i].quantity = 0.1;
        v[i].is_buyer_maker = (i & 1) == 0;
        v[i].timestamp_ns = static_cast<int64_t>(i);
        v[i].trade_id = i;
    }
    return v;
}

// Trades that always cross threshold — strategy resets reference each tick,
// so we step ±(2.5 * threshold_pct) alternating direction.
std::vector<TradeEvent> gen_trades_hot(size_t n, const char* ticker, double threshold_pct) {
    std::vector<TradeEvent> v(n);
    const double step = threshold_pct * 2.5 / 100.0;
    double price = 50000.0;
    int direction = 1;
    for (size_t i = 0; i < n; ++i) {
        copy_ticker(ticker, v[i].ticker, sizeof(v[i].ticker));
        price *= (1.0 + direction * step);
        direction = -direction;
        // Clamp to keep price bounded (long benches could drift wildly).
        if (price > 1e9 || price < 1e-3) price = 50000.0;
        v[i].price = price;
        v[i].quantity = 0.1;
        v[i].is_buyer_maker = (i & 1) == 0;
        v[i].timestamp_ns = static_cast<int64_t>(i);
        v[i].trade_id = i;
    }
    return v;
}

std::vector<QuoteUpdate> gen_quotes(size_t n, const char* ticker) {
    std::vector<QuoteUpdate> v(n);
    std::mt19937_64 rng(424242);
    std::uniform_real_distribution<double> noise(-0.5, 0.5);
    const double base = 50000.0;
    for (size_t i = 0; i < n; ++i) {
        copy_ticker(ticker, v[i].ticker, sizeof(v[i].ticker));
        const double mid = base + noise(rng);
        v[i].bid_price = mid - 0.25;
        v[i].bid_size = 1.0;
        v[i].ask_price = mid + 0.25;
        v[i].ask_size = 1.0;
        v[i].timestamp_ns = static_cast<int64_t>(i);
    }
    return v;
}

}  // namespace

// Run a benchmark: dispatch one event at a time, time each with rdtsc.
template <typename DispatchFn, typename Events>
Stats run_per_call(DispatchFn&& dispatch, const Events& events,
                   size_t warmup, size_t iters,
                   double ns_per_tick, int64_t overhead_ticks) {
    for (size_t i = 0; i < warmup; ++i) dispatch(events[i]);

    std::vector<int64_t> ticks(iters);
    for (size_t i = 0; i < iters; ++i) {
        const auto& e = events[warmup + i];
        BENCH_BARRIER();
        const uint64_t a = rdtsc_start();
        dispatch(e);
        const uint64_t b = rdtsc_end();
        BENCH_BARRIER();
        ticks[i] = static_cast<int64_t>(b - a);
    }
    return compute_stats(ticks, ns_per_tick, overhead_ticks);
}

// Run a batched throughput benchmark: time K events between clock reads,
// repeat M times, return mean ns/event across the bench.
template <typename DispatchFn, typename Events>
double run_batched_mean_ns(DispatchFn&& dispatch, const Events& events,
                           size_t warmup, size_t batches, size_t batch_size,
                           double ns_per_tick) {
    for (size_t i = 0; i < warmup; ++i) dispatch(events[i]);

    const uint64_t a = rdtsc_start();
    size_t idx = warmup;
    for (size_t b = 0; b < batches; ++b) {
        for (size_t k = 0; k < batch_size; ++k) {
            dispatch(events[idx++]);
        }
    }
    const uint64_t e = rdtsc_end();
    const double total_ns = static_cast<double>(e - a) * ns_per_tick;
    return total_ns / static_cast<double>(batches * batch_size);
}

int main() {
    constexpr size_t kWarmup = 20'000;
    constexpr size_t kIters  = 500'000;
    constexpr size_t kBatchSize = 256;
    constexpr size_t kBatches   = 4'000;  // 256 * 4000 = 1.024M events
    const char* ticker = "BTC_USDT";

    std::printf("Hot path microbenchmark — strategy on_event -> order_ready\n");
    std::printf("(excludes rate limiter and network I/O)\n");
    std::printf("Warmup: %zu  Per-call iterations: %zu  Batched: %zu x %zu = %zu events\n",
                kWarmup, kIters, kBatches, kBatchSize, kBatches * kBatchSize);
#if defined(__GNUC__) && !defined(__clang__)
    std::printf("Compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__clang__)
    std::printf("Compiler: clang %d.%d.%d\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
    std::printf("Compiler: MSVC %d\n", _MSC_VER);
#endif

    const double ns_per_tick = calibrate_ns_per_tick();
    const int64_t overhead_ticks = estimate_timer_overhead_ticks(20'000);
    std::printf("TSC calibration: %.4f ns/tick (~%.2f GHz nominal)\n",
                ns_per_tick, 1.0 / ns_per_tick);
    std::printf("Timer overhead: %lld ticks ≈ %.1f ns (subtracted from per-call samples)\n\n",
                (long long)overhead_ticks,
                static_cast<double>(overhead_ticks) * ns_per_tick);

    print_header();

    const size_t total_events = kIters + kWarmup;
    const size_t total_batch_events = kBatches * kBatchSize + kWarmup;
    const size_t max_events = std::max(total_events, total_batch_events);

    double batch_mean_1 = 0, batch_mean_2 = 0, batch_mean_3 = 0;

    // 1. SimpleMomentum, no orders.
    {
        const auto trades = gen_trades_cold(max_events, ticker);

        NoopSink sink_a;
        StrategyManager mgr_a(&sink_a);
        (void)mgr_a.add_strategy<SimpleMomentum>(1u, std::string(ticker), 1.0, 1.0);
        mgr_a.start_all();
        auto disp_a = [&](const TradeEvent& t) { mgr_a.dispatch_trade(t); };
        const auto s = run_per_call(disp_a, trades, kWarmup, kIters, ns_per_tick, overhead_ticks);
        print_stats("SimpleMomentum | trade, NO order", s, sink_a.submitted);

        NoopSink sink_b;
        StrategyManager mgr_b(&sink_b);
        (void)mgr_b.add_strategy<SimpleMomentum>(11u, std::string(ticker), 1.0, 1.0);
        mgr_b.start_all();
        auto disp_b = [&](const TradeEvent& t) { mgr_b.dispatch_trade(t); };
        batch_mean_1 = run_batched_mean_ns(disp_b, trades, kWarmup, kBatches, kBatchSize, ns_per_tick);
    }

    // 2. SimpleMomentum, order every tick.
    {
        const auto trades = gen_trades_hot(max_events, ticker, 1.0);

        NoopSink sink_a;
        StrategyManager mgr_a(&sink_a);
        (void)mgr_a.add_strategy<SimpleMomentum>(2u, std::string(ticker), 1.0, 1.0);
        mgr_a.start_all();
        auto disp_a = [&](const TradeEvent& t) { mgr_a.dispatch_trade(t); };
        const auto s = run_per_call(disp_a, trades, kWarmup, kIters, ns_per_tick, overhead_ticks);
        print_stats("SimpleMomentum | trade, ORDER generated", s, sink_a.submitted);

        NoopSink sink_b;
        StrategyManager mgr_b(&sink_b);
        (void)mgr_b.add_strategy<SimpleMomentum>(22u, std::string(ticker), 1.0, 1.0);
        mgr_b.start_all();
        auto disp_b = [&](const TradeEvent& t) { mgr_b.dispatch_trade(t); };
        batch_mean_2 = run_batched_mean_ns(disp_b, trades, kWarmup, kBatches, kBatchSize, ns_per_tick);
    }

    // 3. AlwaysOrder on quotes.
    {
        const auto quotes = gen_quotes(max_events, ticker);

        NoopSink sink_a;
        StrategyManager mgr_a(&sink_a);
        (void)mgr_a.add_strategy<AlwaysQuoteOrderStrategy>(3u, std::string(ticker));
        mgr_a.start_all();
        auto disp_a = [&](const QuoteUpdate& q) { mgr_a.dispatch_quote(q); };
        const auto s = run_per_call(disp_a, quotes, kWarmup, kIters, ns_per_tick, overhead_ticks);
        print_stats("AlwaysOrder    | quote, ORDER generated", s, sink_a.submitted);

        NoopSink sink_b;
        StrategyManager mgr_b(&sink_b);
        (void)mgr_b.add_strategy<AlwaysQuoteOrderStrategy>(33u, std::string(ticker));
        mgr_b.start_all();
        auto disp_b = [&](const QuoteUpdate& q) { mgr_b.dispatch_quote(q); };
        batch_mean_3 = run_batched_mean_ns(disp_b, quotes, kWarmup, kBatches, kBatchSize, ns_per_tick);
    }

    std::printf("\nBatched mean (no timer overhead per call — most accurate central tendency):\n");
    std::printf("  SimpleMomentum  | trade, NO order            : %7.2f ns/event\n", batch_mean_1);
    std::printf("  SimpleMomentum  | trade, ORDER generated     : %7.2f ns/event\n", batch_mean_2);
    std::printf("  AlwaysOrder     | quote, ORDER generated     : %7.2f ns/event\n", batch_mean_3);

    std::printf("\nNotes:\n");
    std::printf("  * Per-call samples timed with rdtsc (~%.1f ns clock-read overhead, subtracted).\n",
                static_cast<double>(overhead_ticks) * ns_per_tick);
    std::printf("  * Batched mean is the most defensible average — single clock read per %zu events.\n",
                kBatchSize);
    std::printf("  * Max latencies reflect OS scheduling jitter on a non-isolated dev machine.\n");
    std::printf("  * Measured on dev machine, Windows + MinGW GCC, not production-tuned.\n");
    return 0;
}
