#pragma once
#include "types.hpp"
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace lob {

// ── Avellaneda-Stoikov Market Making Strategy ──────────────────────────────
//
// Reference: Avellaneda & Stoikov (2008), "High-frequency trading in a limit
//            order book", Quantitative Finance 8(3), pp. 217-224.
//
// Core formulae:
//   Reservation price:  r*(t) = s(t) − q·γ·σ²·(T−t)
//   Optimal spread:     δ*(t) = γ·σ²·(T−t) + (2/γ)·ln(1 + γ/κ)
//   Bid quote:          b*(t) = r*(t) − δ*(t)/2
//   Ask quote:          a*(t) = r*(t) + δ*(t)/2
//
// Parameters:
//   γ  (gamma) — risk-aversion coefficient  [0.01 – 1.0]
//   κ  (kappa) — order arrival intensity     [proxy for market depth]
//   σ  (sigma) — mid-price volatility (realised, updated each tick)
//   T          — session length in seconds
//   t          — time elapsed in seconds
//   q          — current inventory in base asset (BTC)
class AvellanedaStoikov {
public:
  struct Params {
    double gamma = 0.10; // risk aversion
    double kappa = 1.50; // order arrival intensity
    double T = 60.0;     // session length (seconds)
  };

  struct Quotes {
    double reservation_price;
    double optimal_spread;
    double bid;
    double ask;
  };

  struct SessionStats {
    double realized_pnl = 0.0; // maker half-spread earned
    double adverse_sel_cost =
        0.0; // mid-price drift against inventory post-fill
    double net_pnl = 0.0;
    int total_fills = 0;
  };

  explicit AvellanedaStoikov(Params p = {}) : params_(p) {}

  // ── Quote generation ──────────────────────────────────────────────────
  // Call on every book update. Returns bid/ask quotes to post.
  Quotes compute(double mid, double sigma, double inventory,
                 double time_elapsed) const {
    const double time_left = std::max(params_.T - time_elapsed, 0.001);

    const double r_star =
        mid - inventory * params_.gamma * sigma * sigma * time_left;

    // Correct A-S optimal spread (includes κ term — often omitted incorrectly)
    const double delta_star =
        params_.gamma * sigma * sigma * time_left +
        (2.0 / params_.gamma) * std::log(1.0 + params_.gamma / params_.kappa);

    return Quotes{r_star, delta_star, r_star - delta_star / 2.0,
                  r_star + delta_star / 2.0};
  }

  // ── Fill accounting ───────────────────────────────────────────────────
  // Call when a passive fill occurs.
  //   fill_side    — our side (the side we posted)
  //   fill_price   — matched price
  //   fill_qty     — quantity filled
  //   mid_at_fill  — mid-price at time of fill
  void record_fill(Side fill_side, double fill_price, double fill_qty,
                   double spread_at_fill, double mid_at_fill) {
    // Realized P&L: passive maker earns half the spread per fill
    stats_.realized_pnl += fill_qty * (spread_at_fill / 2.0);

    // Adverse selection cost: measured as mid-price move against our position
    // after the fill. We snapshot mid at fill and compare at next tick.
    last_fill_mid_ = mid_at_fill;
    last_fill_side_ = fill_side;
    last_fill_qty_ = fill_qty;
    pending_adv_ = true;

    stats_.total_fills++;
  }

  // Call on each mid-price tick to measure adverse selection from last fill.
  void tick(double current_mid) {
    if (!pending_adv_)
      return;
    const double move = current_mid - last_fill_mid_;
    // Sold (ask fill) → adverse if mid went up; bought (bid fill) → adverse if
    // mid went down
    double adv = (last_fill_side_ == Side::Sell)
                     ? std::max(0.0, move) * last_fill_qty_
                     : std::max(0.0, -move) * last_fill_qty_;
    stats_.adverse_sel_cost += adv;
    stats_.net_pnl = stats_.realized_pnl - stats_.adverse_sel_cost;
    pending_adv_ = false;
  }

  // ── Volatility estimator ──────────────────────────────────────────────
  // EWMA realised volatility. Call with each new mid-price observation.
  // Returns updated σ. alpha=0.06 → ~15-observation half-life.
  double update_sigma(double new_mid, double prev_mid, double alpha = 0.06) {
    if (prev_mid <= 0.0)
      return sigma_;
    const double ret = (new_mid - prev_mid) / prev_mid;
    sigma_ = std::sqrt((1.0 - alpha) * sigma_ * sigma_ + alpha * ret * ret);
    sigma_ = std::max(0.0001, std::min(0.50, sigma_));
    return sigma_;
  }

  double sigma() const { return sigma_; }
  const SessionStats &stats() const { return stats_; }
  const Params &params() const { return params_; }

  void reset_session() {
    stats_ = {};
    pending_adv_ = false;
  }

private:
  Params params_;
  SessionStats stats_;
  double sigma_ = 0.018;
  double last_fill_mid_ = 0.0;
  double last_fill_qty_ = 0.0;
  Side last_fill_side_ = Side::Buy;
  bool pending_adv_ = false;
};

} // namespace lob