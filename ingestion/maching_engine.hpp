#pragma once
#include "../aligned/spsc_queue.hpp"
#include "../include/types.hpp"
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace lob {

// ── Price level ────────────────────────────────────────────────────────────
// A doubly-linked list of orders at the same price gives O(1) FIFO removal,
// preserving time priority within a price level.
struct PriceLevel {
  Price price;
  Quantity total_qty = 0;
  std::list<Order> orders; // front = oldest (highest priority)
};

// ── Order book side ────────────────────────────────────────────────────────
// Bids: descending price (best bid = highest price = begin())
// Asks: ascending  price (best ask = lowest  price = begin())
template <typename Compare>
using PriceLevelMap = std::map<Price, PriceLevel, Compare>;

using BidLevels = PriceLevelMap<std::greater<Price>>;
using AskLevels = PriceLevelMap<std::less<Price>>;

// ── Matching engine ────────────────────────────────────────────────────────
class MatchingEngine {
public:
  // Capacity = 64K orders in the ingestion queue (power of two)
  static constexpr std::size_t QUEUE_CAPACITY = 1 << 16;

  using TradeCallback = std::function<void(const Trade &)>;

  explicit MatchingEngine(TradeCallback on_trade = nullptr);

  // ── Public API (called from network/gateway thread) ──────────────────
  bool submit(const Order &order); // enqueue; returns false if queue full
  void cancel(OrderId id);

  // ── Match loop (called from engine thread in tight loop) ──────────────
  // Drains the ingestion queue and matches. Returns number of orders processed.
  int process_pending();

  // ── Book inspection ───────────────────────────────────────────────────
  Price best_bid() const;
  Price best_ask() const;
  double mid_price() const;
  double spread() const;
  double order_book_imbalance(int levels = 3) const;

  const BidLevels &bids() const { return bids_; }
  const AskLevels &asks() const { return asks_; }

  uint64_t total_trades() const { return total_trades_; }
  uint64_t total_orders() const { return total_orders_; }

private:
  void add_order(Order &order);
  void match_order(Order &taker);
  void match_buy(Order &taker);
  void match_sell(Order &taker);
  void remove_order(OrderId id);

  static uint64_t now_ns();

  // ── Book state ────────────────────────────────────────────────────────
  BidLevels bids_;
  AskLevels asks_;

  // order_id → iterator into the list in its PriceLevel (O(1) cancel)
  struct OrderLocation {
    Side side;
    Price price;
    std::list<Order>::iterator it;
  };
  std::unordered_map<OrderId, OrderLocation> order_map_;

  // ── Lock-free ingestion queue ─────────────────────────────────────────
  SPSCQueue<Order, QUEUE_CAPACITY> ingest_queue_;

  // ── Stats ─────────────────────────────────────────────────────────────
  uint64_t total_trades_ = 0;
  uint64_t total_orders_ = 0;
  TradeCallback on_trade_;
};

} // namespace lob