#pragma once
#include <cstdint>
#include <string>

namespace lob {

// ── Enums ────────────────────────────────────────────────
enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit = 0, Market = 1 };
enum class OrderStatus : uint8_t { New, PartialFill, Filled, Cancelled };

// ── Price & quantity representation ──────────────────────
// Store price as integer ticks (1 tick = $0.01) to avoid float comparison bugs.
using Price = int64_t;    // e.g. $67421.50 → 6742150
using Quantity = int64_t; // e.g. 1.5 BTC  → 15000 (4 decimal places)
using OrderId = uint64_t;
using SeqNum = uint64_t; // Coinbase WS sequence number

constexpr Price PRICE_SCALE = 100;    // 2 decimal places for USD
constexpr Quantity QTY_SCALE = 10000; // 4 decimal places for BTC

inline Price to_price(double p) {
  return static_cast<Price>(p * PRICE_SCALE + 0.5);
}
inline Quantity to_qty(double q) {
  return static_cast<Quantity>(q * QTY_SCALE + 0.5);
}
inline double from_price(Price p) {
  return static_cast<double>(p) / PRICE_SCALE;
}
inline double from_qty(Quantity q) {
  return static_cast<double>(q) / QTY_SCALE;
}

// ── Order ─────────────────────────────────────────────────
// Cache-line aligned (64 bytes) to eliminate false sharing on the match path.
struct alignas(64) Order {
  OrderId id;
  Price price;
  Quantity quantity;
  Quantity remaining;
  Side side;
  OrderType type;
  OrderStatus status;
  uint64_t timestamp_ns; // arrival time for price-time priority
  uint8_t _pad[64 - sizeof(OrderId) - sizeof(Price) - sizeof(Quantity) * 2 -
               sizeof(Side) - sizeof(OrderType) - sizeof(OrderStatus) -
               sizeof(uint64_t)];
};
static_assert(sizeof(Order) == 64, "Order must be exactly one cache line");

// ── Trade (fill) ──────────────────────────────────────────
struct Trade {
  OrderId maker_order_id;
  OrderId taker_order_id;
  Price price;
  Quantity quantity;
  Side aggressor; // which side crossed the spread
  uint64_t timestamp_ns;
  uint64_t latency_ns; // time from order receipt to match
};

// ── L2 update (from WebSocket feed) ──────────────────────
enum class UpdateAction : uint8_t { Add, Change, Delete };

struct L2Update {
  SeqNum sequence;
  Side side;
  Price price;
  Quantity size; // 0 = delete this level
  UpdateAction action;
  uint64_t timestamp_ns;
};

} // namespace lob