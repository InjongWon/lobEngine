#include "matching_engine.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace lob {

// ── Constructor ────────────────────────────────────────────────────────────
MatchingEngine::MatchingEngine(TradeCallback on_trade)
    : on_trade_(std::move(on_trade)) {}

// ── Ingestion (producer side — network thread) ─────────────────────────────
bool MatchingEngine::submit(const Order &order) {
  return ingest_queue_.push(order);
}

// ── Process pending (consumer side — engine thread) ───────────────────────
// This is the hot path. Keep it tight.
int MatchingEngine::process_pending() {
  int n = 0;
  while (auto opt = ingest_queue_.pop()) {
    add_order(*opt);
    ++n;
  }
  return n;
}

// ── Add order to book or match immediately ─────────────────────────────────
void MatchingEngine::add_order(Order &order) {
  ++total_orders_;

  if (order.type == OrderType::Market) {
    match_order(order);
    return;
  }

  // Try to match first (aggressive limit order crosses the spread)
  match_order(order);

  // If remaining quantity: rest it in the book
  if (order.remaining > 0 && order.status != OrderStatus::Cancelled) {
    order.status = (order.remaining < order.quantity) ? OrderStatus::PartialFill
                                                      : OrderStatus::New;

    auto &levels =
        (order.side == Side::Buy)
            ? reinterpret_cast<
                  std::map<Price, PriceLevel, std::greater<Price>> &>(bids_)
            : reinterpret_cast<std::map<Price, PriceLevel, std::less<Price>> &>(
                  asks_);

    if (order.side == Side::Buy) {
      auto &level = bids_[order.price];
      level.price = order.price;
      level.total_qty += order.remaining;
      level.orders.push_back(order);
      order_map_[order.id] = {Side::Buy, order.price,
                              std::prev(level.orders.end())};
    } else {
      auto &level = asks_[order.price];
      level.price = order.price;
      level.total_qty += order.remaining;
      level.orders.push_back(order);
      order_map_[order.id] = {Side::Sell, order.price,
                              std::prev(level.orders.end())};
    }
  }
}

// ── Core matching logic ────────────────────────────────────────────────────
void MatchingEngine::match_order(Order &taker) {
  if (taker.side == Side::Buy)
    match_buy(taker);
  else
    match_sell(taker);
}

// Buy taker crosses against best asks (ascending price)
void MatchingEngine::match_buy(Order &taker) {
  const uint64_t t0 = now_ns();

  while (taker.remaining > 0 && !asks_.empty()) {
    auto &[best_price, level] = *asks_.begin();

    // Price check: limit orders only match if ask <= bid limit
    if (taker.type == OrderType::Limit && best_price > taker.price)
      break;

    // Walk the queue at this price level (FIFO = time priority)
    while (taker.remaining > 0 && !level.orders.empty()) {
      Order &maker = level.orders.front();

      const Quantity fill_qty = std::min(taker.remaining, maker.remaining);
      const Price fill_px = maker.price; // maker price wins

      taker.remaining -= fill_qty;
      maker.remaining -= fill_qty;
      level.total_qty -= fill_qty;

      if (taker.remaining == 0)
        taker.status = OrderStatus::Filled;
      if (maker.remaining == 0) {
        maker.status = OrderStatus::Filled;
        order_map_.erase(maker.id);
        level.orders.pop_front();
      }

      ++total_trades_;
      if (on_trade_) {
        on_trade_(Trade{maker.id, taker.id, fill_px, fill_qty, Side::Buy,
                        now_ns(), now_ns() - t0});
      }
    }

    if (level.orders.empty())
      asks_.erase(asks_.begin());
  }
}

// Sell taker crosses against best bids (descending price)
void MatchingEngine::match_sell(Order &taker) {
  const uint64_t t0 = now_ns();

  while (taker.remaining > 0 && !bids_.empty()) {
    auto &[best_price, level] = *bids_.begin();

    if (taker.type == OrderType::Limit && best_price < taker.price)
      break;

    while (taker.remaining > 0 && !level.orders.empty()) {
      Order &maker = level.orders.front();

      const Quantity fill_qty = std::min(taker.remaining, maker.remaining);
      const Price fill_px = maker.price;

      taker.remaining -= fill_qty;
      maker.remaining -= fill_qty;
      level.total_qty -= fill_qty;

      if (taker.remaining == 0)
        taker.status = OrderStatus::Filled;
      if (maker.remaining == 0) {
        maker.status = OrderStatus::Filled;
        order_map_.erase(maker.id);
        level.orders.pop_front();
      }

      ++total_trades_;
      if (on_trade_) {
        on_trade_(Trade{maker.id, taker.id, fill_px, fill_qty, Side::Sell,
                        now_ns(), now_ns() - t0});
      }
    }

    if (level.orders.empty())
      bids_.erase(bids_.begin());
  }
}

// ── Cancel ─────────────────────────────────────────────────────────────────
void MatchingEngine::cancel(OrderId id) {
  auto it = order_map_.find(id);
  if (it == order_map_.end())
    return;

  auto &loc = it->second;
  if (loc.side == Side::Buy) {
    auto &level = bids_.at(loc.price);
    level.total_qty -= loc.it->remaining;
    level.orders.erase(loc.it);
    if (level.orders.empty())
      bids_.erase(loc.price);
  } else {
    auto &level = asks_.at(loc.price);
    level.total_qty -= loc.it->remaining;
    level.orders.erase(loc.it);
    if (level.orders.empty())
      asks_.erase(loc.price);
  }
  order_map_.erase(it);
}

// ── Book signals ───────────────────────────────────────────────────────────
Price MatchingEngine::best_bid() const {
  return bids_.empty() ? 0 : bids_.begin()->first;
}

Price MatchingEngine::best_ask() const {
  return asks_.empty() ? 0 : asks_.begin()->first;
}

double MatchingEngine::mid_price() const {
  if (bids_.empty() || asks_.empty())
    return 0.0;
  return (from_price(best_bid()) + from_price(best_ask())) / 2.0;
}

double MatchingEngine::spread() const {
  if (bids_.empty() || asks_.empty())
    return 0.0;
  return from_price(best_ask() - best_bid());
}

// OBI = (bid_vol - ask_vol) / (bid_vol + ask_vol) over top N levels
double MatchingEngine::order_book_imbalance(int levels) const {
  Quantity bid_vol = 0, ask_vol = 0;
  int n = 0;
  for (auto &[px, lvl] : bids_) {
    bid_vol += lvl.total_qty;
    if (++n >= levels)
      break;
  }
  n = 0;
  for (auto &[px, lvl] : asks_) {
    ask_vol += lvl.total_qty;
    if (++n >= levels)
      break;
  }
  const Quantity total = bid_vol + ask_vol;
  if (total == 0)
    return 0.0;
  return static_cast<double>(bid_vol - ask_vol) / static_cast<double>(total);
}

// ── Timestamp ──────────────────────────────────────────────────────────────
uint64_t MatchingEngine::now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

} // namespace lob