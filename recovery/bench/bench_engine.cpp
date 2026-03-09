#include "avellaneda_stoikov.hpp"
#include "matching_engine.hpp"
#include <benchmark/benchmark.h>
#include <chrono>
#include <random>

using namespace lob;

// ── Helpers ────────────────────────────────────────────────────────────────
static uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

static Order make_order(OrderId id, Side side, double price, double qty,
                        OrderType type = OrderType::Limit) {
  Order o{};
  o.id = id;
  o.side = side;
  o.price = to_price(price);
  o.quantity = to_qty(qty);
  o.remaining = o.quantity;
  o.type = type;
  o.status = OrderStatus::New;
  o.timestamp_ns = now_ns();
  return o;
}

// Seed the book with resting liquidity so matches actually happen
static void seed_book(MatchingEngine &engine, int levels = 10) {
  OrderId id = 1'000'000;
  for (int i = 0; i < levels; ++i) {
    // Asks: 100.10, 100.20, ...
    engine.submit(
        make_order(id++, Side::Sell, 100.10 + i * 0.10, 1.0 + i * 0.1));
    // Bids: 100.00, 99.90, ...
    engine.submit(
        make_order(id++, Side::Buy, 100.00 - i * 0.10, 1.0 + i * 0.1));
  }
  engine.process_pending();
}

// ── BM_MatchEngine_PriceTimePriority ──────────────────────────────────────
// Measures end-to-end latency of a single aggressive limit order that crosses
// the spread and fills against resting liquidity.
// This is the critical match path: price lookup → FIFO queue walk → fill.
static void BM_MatchEngine_PriceTimePriority(benchmark::State &state) {
  MatchingEngine engine;
  seed_book(engine, 20);

  OrderId id = 2'000'000;
  for (auto _ : state) {
    // Aggressive buy: crosses best ask → immediate match
    Order taker = make_order(id++, Side::Buy, 100.50, 0.1);
    engine.submit(taker);
    engine.process_pending();

    // Re-seed consumed level to keep book stable across iterations
    engine.submit(make_order(id++, Side::Sell, 100.10, 0.1));
    engine.process_pending();
  }

  state.SetLabel("price-time priority / lock-free ingestion");
  state.counters["orders/s"] =
      benchmark::Counter(state.iterations() * 2, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_MatchEngine_PriceTimePriority)
    ->MinTime(2.0)
    ->Unit(benchmark::kNanosecond);

// ── BM_LockFree_OrderIngestion ─────────────────────────────────────────────
// Measures just the SPSC enqueue + dequeue round-trip (no matching).
// Isolates lock-free queue overhead from matching overhead.
static void BM_LockFree_OrderIngestion(benchmark::State &state) {
  MatchingEngine engine;
  OrderId id = 3'000'000;

  for (auto _ : state) {
    Order o = make_order(id++, Side::Buy, 100.0, 1.0);
    benchmark::DoNotOptimize(engine.submit(o));
    benchmark::DoNotOptimize(engine.process_pending());
  }

  state.SetLabel("SPSC enqueue+dequeue, cache-line aligned structs");
  state.counters["ops/s"] =
      benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_LockFree_OrderIngestion)
    ->MinTime(2.0)
    ->Unit(benchmark::kNanosecond);

// ── BM_OrderBookImbalance ──────────────────────────────────────────────────
// Measures signal derivation cost — OBI computed over top-3 levels.
static void BM_OrderBookImbalance(benchmark::State &state) {
  MatchingEngine engine;
  seed_book(engine, 10);

  for (auto _ : state) {
    benchmark::DoNotOptimize(engine.order_book_imbalance(3));
  }
  state.SetLabel("OBI top-3 levels");
  state.Unit(benchmark::kNanosecond);
}
BENCHMARK(BM_OrderBookImbalance)->MinTime(1.0)->Unit(benchmark::kNanosecond);

// ── BM_AS_QuoteComputation ─────────────────────────────────────────────────
// Measures A-S reservation price + optimal spread computation.
static void BM_AS_QuoteComputation(benchmark::State &state) {
  AvellanedaStoikov as;
  double mid = 67421.50, sigma = 0.018, inv = 0.5, t = 30.0;

  for (auto _ : state) {
    benchmark::DoNotOptimize(as.compute(mid, sigma, inv, t));
    mid += 0.01;
  }
  state.SetLabel("r*(t) and delta*(t) computation");
  state.Unit(benchmark::kNanosecond);
}
BENCHMARK(BM_AS_QuoteComputation)->MinTime(1.0)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();