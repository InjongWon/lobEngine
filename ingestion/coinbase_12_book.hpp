#pragma once
#include "../include/types.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>

namespace lob {

// ── Coinbase L2 order book ─────────────────────────────────────────────────
// Reconstructs the full L2 book from WebSocket incremental updates.
// Handles sequence-gap detection and recovery.
//
// Coinbase L2 feed protocol:
//   - "snapshot" message gives the initial book state + sequence number
//   - "l2update" messages carry incremental changes (add/change/delete)
//   - Each update has a monotonically increasing sequence number
//   - A gap (missing sequence) means we lost updates → must re-subscribe
class CoinbaseL2Book {
public:
  using GapCallback = std::function<void(SeqNum expected, SeqNum got)>;
  using UpdateCallback = std::function<void(const L2Update &)>;

  explicit CoinbaseL2Book(GapCallback on_gap = nullptr,
                          UpdateCallback on_update = nullptr);

  // ── Feed ingestion ────────────────────────────────────────────────────

  // Call once with the snapshot message to initialise the book.
  void apply_snapshot(SeqNum seq,
                      const std::vector<std::pair<double, double>> &bids,
                      const std::vector<std::pair<double, double>> &asks);

  // Call for every l2update message received from the WebSocket.
  // Returns false if a sequence gap was detected (book state is stale).
  bool apply_update(const L2Update &upd);

  // ── Gap recovery ──────────────────────────────────────────────────────
  // Call this after re-subscribing and receiving a fresh snapshot.
  void reset() {
    initialised_ = false;
    expected_seq_ = 0;
    bids_.clear();
    asks_.clear();
  }
  bool is_initialised() const { return initialised_; }
  uint64_t gaps_recovered() const { return gaps_recovered_; }

  // ── Book access ───────────────────────────────────────────────────────
  // Best bid (highest buy price), best ask (lowest sell price)
  double best_bid() const;
  double best_ask() const;
  double mid_price() const;
  double spread() const;

  // OBI = (bid_vol - ask_vol) / (bid_vol + ask_vol) over top N levels
  double order_book_imbalance(int levels = 3) const;

  // Print top N levels to stdout (for debugging / demo)
  void print(int levels = 5) const;

private:
  // price → size (integer ticks)
  std::map<Price, Quantity, std::greater<Price>> bids_; // best bid = begin()
  std::map<Price, Quantity, std::less<Price>> asks_;    // best ask = begin()

  SeqNum expected_seq_ = 0;
  bool initialised_ = false;
  uint64_t gaps_recovered_ = 0;

  GapCallback on_gap_;
  UpdateCallback on_update_;

  void apply_side_update(const L2Update &upd);
};

} // namespace lob