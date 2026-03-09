#include "avellaneda_stoikov.hpp"
#include "coinbase_l2_book.hpp"
#include "matching_engine.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

using namespace lob;

// ── Simulated session ──────────────────────────────────────────────────────
// Replays a market making session using the matching engine + A-S strategy.
// In production this feeds from a real Coinbase WebSocket; here we drive it
// with a Geometric Brownian Motion price process.
int main() {
  std::cout << "╔══════════════════════════════════════════════════════╗\n"
            << "║  LOB Engine — Avellaneda-Stoikov Simulated Session   ║\n"
            << "║  C++17 · lock-free · price-time priority             ║\n"
            << "╚══════════════════════════════════════════════════════╝\n\n";

  // ── Engine & strategy setup ──────────────────────────────────────────
  int fill_count = 0;
  MatchingEngine engine([&](const Trade &t) {
    ++fill_count;
    std::cout << std::fixed << std::setprecision(2) << "  FILL  "
              << (t.aggressor == Side::Buy ? "BUY " : "SELL")
              << "  px=" << from_price(t.price)
              << "  qty=" << std::setprecision(4) << from_qty(t.quantity)
              << "  lat=" << t.latency_ns << "ns\n";
  });

  CoinbaseL2Book l2book([](SeqNum exp, SeqNum got) {
    std::cerr << "  ⚠ SEQ GAP: expected=" << exp << " got=" << got
              << " → recovering\n";
  });

  AvellanedaStoikov as({.gamma = 0.10, .kappa = 1.50, .T = 60.0});

  // ── GBM price process ─────────────────────────────────────────────────
  std::mt19937_64 rng(42);
  std::normal_distribution<> norm(0.0, 1.0);

  double mid = 67421.50;
  double prev_mid = mid;
  double sigma = 0.018;
  double dt = 0.1; // 100ms steps
  double inventory = 0.0;

  // Seed with a snapshot
  l2book.apply_snapshot(
      4200000, {{mid - 0.50, 1.0}, {mid - 1.00, 2.0}, {mid - 1.50, 3.0}},
      {{mid + 0.50, 1.0}, {mid + 1.00, 2.0}, {mid + 1.50, 3.0}});

  OrderId next_id = 1;
  SeqNum seq = 4200001;

  const int STEPS = 600; // 60 seconds
  std::cout << std::fixed;

  for (int step = 0; step < STEPS; ++step) {
    const double t_elapsed = step * dt;

    // ── GBM price step ────────────────────────────────────────────────
    prev_mid = mid;
    mid += sigma * mid * norm(rng) * std::sqrt(dt) * 0.01;
    mid = std::max(mid, 60000.0);
    sigma = as.update_sigma(mid, prev_mid);
    as.tick(mid);

    // ── Simulate incremental L2 feed update ──────────────────────────
    const double spread = 0.30 + std::abs(norm(rng)) * 0.20;
    L2Update upd;
    upd.sequence = seq++;
    upd.side = (step % 2 == 0) ? Side::Buy : Side::Sell;
    upd.price =
        to_price(upd.side == Side::Buy ? mid - spread / 2 : mid + spread / 2);
    upd.size = to_qty(0.5 + std::abs(norm(rng)) * 1.5);
    upd.action = UpdateAction::Change;
    upd.timestamp_ns = 0;
    l2book.apply_update(upd);

    // ── A-S quotes ────────────────────────────────────────────────────
    auto q = as.compute(mid, sigma, inventory, t_elapsed);

    // ── Post our quotes into the matching engine ───────────────────────
    Order bid_order = {};
    bid_order.id = next_id++;
    bid_order.side = Side::Buy;
    bid_order.price = to_price(q.bid);
    bid_order.quantity = to_qty(0.1);
    bid_order.remaining = bid_order.quantity;
    bid_order.type = OrderType::Limit;
    bid_order.status = OrderStatus::New;
    engine.submit(bid_order);

    Order ask_order = {};
    ask_order.id = next_id++;
    ask_order.side = Side::Sell;
    ask_order.price = to_price(q.ask);
    ask_order.quantity = to_qty(0.1);
    ask_order.remaining = ask_order.quantity;
    ask_order.type = OrderType::Limit;
    ask_order.status = OrderStatus::New;
    engine.submit(ask_order);

    // ── Simulate incoming market taker ────────────────────────────────
    if (step % 5 == 0) {
      bool taker_buys = norm(rng) > 0;
      Order taker = {};
      taker.id = next_id++;
      taker.side = taker_buys ? Side::Buy : Side::Sell;
      taker.price = to_price(taker_buys ? q.ask + 0.10 : q.bid - 0.10);
      taker.quantity = to_qty(0.1);
      taker.remaining = taker.quantity;
      taker.type = OrderType::Limit;
      taker.status = OrderStatus::New;
      engine.submit(taker);

      // Inventory changes when we're filled by the taker
      const double fill_qty = 0.1;
      inventory += taker_buys ? -fill_qty : fill_qty;
      inventory = std::max(-10.0, std::min(10.0, inventory));

      as.record_fill(taker_buys ? Side::Sell : Side::Buy,
                     taker_buys ? q.ask : q.bid, fill_qty, spread, mid);
    }

    engine.process_pending();

    // ── Print status every 10 steps (1 second) ────────────────────────
    if (step % 10 == 0) {
      const auto &s = as.stats();
      std::cout << "\n[t=" << std::setw(5) << std::setprecision(1) << t_elapsed
                << "s]"
                << "  mid=" << std::setprecision(2) << mid
                << "  σ=" << std::setprecision(4) << sigma
                << "  inv=" << std::setprecision(3) << inventory << " BTC"
                << "\n  r*=" << std::setprecision(2) << q.reservation_price
                << "  δ*=" << std::setprecision(4) << q.optimal_spread
                << "  bid=" << q.bid << "  ask=" << q.ask
                << "\n  P&L: realized=+$" << std::setprecision(3)
                << s.realized_pnl << "  adv_sel=-$" << s.adverse_sel_cost
                << "  net=" << (s.net_pnl >= 0 ? "+$" : "-$")
                << std::abs(s.net_pnl) << "  fills=" << s.total_fills
                << "  gaps=" << l2book.gaps_recovered() << "\n";
    }
  }

  // ── Session summary ────────────────────────────────────────────────────
  const auto &s = as.stats();
  std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
            << "║                  SESSION SUMMARY                     ║\n"
            << "╠══════════════════════════════════════════════════════╣\n"
            << "║  Total fills:        " << std::setw(6) << s.total_fills
            << "                        ║\n"
            << "║  Total engine orders:" << std::setw(6)
            << engine.total_orders() << "                        ║\n"
            << "║  Realized P&L:      +$" << std::setprecision(4)
            << s.realized_pnl << "                    ║\n"
            << "║  Adverse sel cost:  -$" << s.adverse_sel_cost
            << "                    ║\n"
            << "║  Net P&L:            " << (s.net_pnl >= 0 ? "+$" : "-$")
            << std::abs(s.net_pnl) << "                    ║\n"
            << "║  Seq gaps recovered: " << std::setw(6)
            << l2book.gaps_recovered() << "                        ║\n"
            << "╚══════════════════════════════════════════════════════╝\n";

  return 0;
}