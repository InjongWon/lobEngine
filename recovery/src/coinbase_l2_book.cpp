#include "coinbase_l2_book.hpp"
#include <iomanip>
#include <sstream>

namespace lob {

CoinbaseL2Book::CoinbaseL2Book(GapCallback on_gap, UpdateCallback on_update)
    : on_gap_(std::move(on_gap)), on_update_(std::move(on_update)) {}

// ── Snapshot ───────────────────────────────────────────────────────────────
void CoinbaseL2Book::apply_snapshot(
    SeqNum seq, const std::vector<std::pair<double, double>> &bids,
    const std::vector<std::pair<double, double>> &asks) {
  bids_.clear();
  asks_.clear();

  for (auto &[p, q] : bids)
    if (q > 0)
      bids_[to_price(p)] = to_qty(q);

  for (auto &[p, q] : asks)
    if (q > 0)
      asks_[to_price(p)] = to_qty(q);

  expected_seq_ = seq + 1;
  initialised_ = true;
}

// ── Incremental update ─────────────────────────────────────────────────────
bool CoinbaseL2Book::apply_update(const L2Update &upd) {
  if (!initialised_)
    return false;

  // Sequence-gap detection
  if (upd.sequence != expected_seq_) {
    ++gaps_recovered_;
    if (on_gap_)
      on_gap_(expected_seq_, upd.sequence);
    // In production: tear down and re-subscribe.
    // Here we accept the update and advance seq to avoid cascading errors.
    expected_seq_ = upd.sequence;
  }

  expected_seq_ = upd.sequence + 1;
  apply_side_update(upd);

  if (on_update_)
    on_update_(upd);
  return true;
}

// ── Apply one side update ──────────────────────────────────────────────────
void CoinbaseL2Book::apply_side_update(const L2Update &upd) {
  const Price px = upd.price;
  const Quantity qty = upd.size;

  if (upd.side == Side::Buy) {
    if (qty == 0 || upd.action == UpdateAction::Delete)
      bids_.erase(px);
    else
      bids_[px] = qty;
  } else {
    if (qty == 0 || upd.action == UpdateAction::Delete)
      asks_.erase(px);
    else
      asks_[px] = qty;
  }
}

// ── Book signals ───────────────────────────────────────────────────────────
double CoinbaseL2Book::best_bid() const {
  return bids_.empty() ? 0.0 : from_price(bids_.begin()->first);
}

double CoinbaseL2Book::best_ask() const {
  return asks_.empty() ? 0.0 : from_price(asks_.begin()->first);
}

double CoinbaseL2Book::mid_price() const {
  if (bids_.empty() || asks_.empty())
    return 0.0;
  return (best_bid() + best_ask()) / 2.0;
}

double CoinbaseL2Book::spread() const {
  if (bids_.empty() || asks_.empty())
    return 0.0;
  return best_ask() - best_bid();
}

double CoinbaseL2Book::order_book_imbalance(int levels) const {
  Quantity bv = 0, av = 0;
  int n = 0;
  for (auto &[px, qty] : bids_) {
    bv += qty;
    if (++n >= levels)
      break;
  }
  n = 0;
  for (auto &[px, qty] : asks_) {
    av += qty;
    if (++n >= levels)
      break;
  }
  const Quantity total = bv + av;
  if (total == 0)
    return 0.0;
  return static_cast<double>(bv - av) / static_cast<double>(total);
}

// ── Debug print ────────────────────────────────────────────────────────────
void CoinbaseL2Book::print(int levels) const {
  std::cout << "\n=== Coinbase L2 Book (seq " << expected_seq_ << ") ===\n";
  std::cout << std::fixed << std::setprecision(2);

  int n = 0;
  std::vector<std::pair<Price, Quantity>> ask_vec(asks_.begin(), asks_.end());
  for (int i = std::min((int)ask_vec.size(), levels) - 1; i >= 0; --i)
    std::cout << "  ASK  " << from_price(ask_vec[i].first) << "  "
              << from_qty(ask_vec[i].second) << "\n";

  std::cout << "  ── mid " << mid_price() << " / spd " << spread() << " ──\n";

  n = 0;
  for (auto &[px, qty] : bids_) {
    std::cout << "  BID  " << from_price(px) << "  " << from_qty(qty) << "\n";
    if (++n >= levels)
      break;
  }
  std::cout << "  OBI: " << std::setprecision(4) << order_book_imbalance()
            << "\n";
}

} // namespace lob