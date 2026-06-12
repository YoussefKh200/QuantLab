#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/execution/OrderBook.h
// Intra-bar tick simulation and simulated limit order book.
// Ported from QuantEngine v2, integrated into ql:: namespace.
//
// IntraBarSynthesizer: Brownian bridge path O→C constrained to [L,H],
// with volume-distributed ticks and configurable bid-ask spread.
//
// SimulatedOrderBook: 5-level depth model with size decay and level
// spacing. estimate_market_fill() walks the book to compute price impact.
//
// TickExecutionEngine: drop-in replacement for bar-level fills when
// higher intra-bar fill-price accuracy is needed.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/Clock.h"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

namespace ql {

// ── Intra-bar path synthesizer ─────────────────────────────────────────────
struct SynthConfig {
    int      n_ticks      = 100;
    double   spread_bps   = 1.0;
    unsigned seed         = 0;   // 0 = derive from bar timestamp
};

class IntraBarSynthesizer {
public:
    static std::vector<Tick> synthesize(const Bar& bar,
                                         SynthConfig cfg = {}) {
        unsigned seed = (cfg.seed == 0)
            ? static_cast<unsigned>(bar.ts_open & 0xFFFFFFFF)
            : cfg.seed;
        std::mt19937 rng(seed);
        std::normal_distribution<double>           norm(0, 1);
        std::uniform_real_distribution<double>     uni(0, 1);

        const int N = cfg.n_ticks;
        std::vector<double> path(N);
        path[0]   = bar.open;
        path[N-1] = bar.close;

        // Brownian bridge
        for (int i = 1; i < N-1; ++i) {
            double t   = (double)i / (N-1);
            double mu  = bar.open + t*(bar.close - bar.open);
            double var = t*(1-t) * (bar.high - bar.low) * 0.5;
            path[i]    = mu + norm(rng)*std::sqrt(std::max(var, 1e-10));
        }

        // Rescale to honour [low, high] constraint
        double cur_min = *std::min_element(path.begin(), path.end());
        double cur_max = *std::max_element(path.begin(), path.end());
        double cur_rng = cur_max - cur_min;
        double tgt_rng = bar.high - bar.low;
        if (cur_rng > 1e-9) {
            for (auto& p : path)
                p = bar.low + (p - cur_min) / cur_rng * tgt_rng;
        }
        for (auto& p : path) p = std::clamp(p, bar.low, bar.high);

        double half_spread = bar.close * cfg.spread_bps / 20000.0;
        double tick_vol    = bar.volume / N;
        Nanos  tick_ns     = (bar.ts_close - bar.ts_open) / N;

        std::vector<Tick> ticks;
        ticks.reserve(N);
        for (int i = 0; i < N; ++i) {
            Tick t;
            t.instrument   = bar.instrument;
            t.ts           = bar.ts_open + static_cast<Timestamp>(i) * tick_ns;
            t.price        = path[i];
            t.bid          = path[i] - half_spread;
            t.ask          = path[i] + half_spread;
            t.bid_size     = 500.0 + 200*uni(rng);
            t.ask_size     = 500.0 + 200*uni(rng);
            t.size         = tick_vol * (0.5 + 0.5*uni(rng));
            t.is_trade     = true;
            t.aggressor_buy= (uni(rng) > 0.5);
            ticks.push_back(t);
        }
        return ticks;
    }
};

// ── Simulated limit order book (5-level) ──────────────────────────────────
struct LOBLevel {
    double price;
    double qty;
};

struct LOBConfig {
    int    depth_levels     = 5;
    double level_spacing_bps= 1.0;
    double base_size        = 10000.0;
    double size_decay       = 0.70;
};

class SimulatedOrderBook {
public:
    explicit SimulatedOrderBook(LOBConfig cfg = LOBConfig()) : cfg_(cfg) {}

    void update(double mid, double spread) {
        mid_ = mid; spread_ = spread;
        rebuild();
    }

    void update(const Tick& t) {
        update(t.mid(), t.spread());
    }

    struct FillEstimate {
        double avg_price;
        double filled_qty;
        double price_impact;   // vs best level
    };

    FillEstimate estimate_market_fill(OrderSide side, double qty) const {
        auto& levels = (side == OrderSide::Buy ||
                        side == OrderSide::BuyToCover) ? asks_ : bids_;
        double cost = 0, filled = 0, rem = qty;
        for (auto& lv : levels) {
            if (rem <= 0) break;
            double take = std::min(rem, lv.qty);
            cost   += take * lv.price;
            filled += take;
            rem    -= take;
        }
        if (rem > 0 && !levels.empty()) {
            // beyond book — penalty price
            double penalty = levels.back().price * 1.002;
            cost   += rem * penalty;
            filled += rem;
        }
        double avg = (filled > 0) ? cost/filled : mid_;
        double first= levels.empty() ? mid_ : levels[0].price;
        return {avg, filled, std::abs(avg - first)};
    }

    double best_bid() const { return bids_.empty() ? mid_-spread_/2 : bids_[0].price; }
    double best_ask() const { return asks_.empty() ? mid_+spread_/2 : asks_[0].price; }
    double mid()      const { return mid_; }

private:
    void rebuild() {
        bids_.clear(); asks_.clear();
        double sp = cfg_.level_spacing_bps * mid_ / 10000.0;
        for (int i = 0; i < cfg_.depth_levels; ++i) {
            double sz = cfg_.base_size * std::pow(cfg_.size_decay, i);
            bids_.push_back({mid_ - spread_/2 - i*sp, sz});
            asks_.push_back({mid_ + spread_/2 + i*sp, sz});
        }
    }

    LOBConfig cfg_;
    double mid_ = 0, spread_ = 0;
    std::vector<LOBLevel> bids_, asks_;
};

// ── Tick-level execution engine ────────────────────────────────────────────
// Replaces bar-level fill logic when tick accuracy is needed.
struct TickExecConfig {
    int    ticks_per_bar        = 100;
    double spread_bps           = 1.0;
    double commission_per_share = 0.005;
};

class TickExecutionEngine {
public:
    explicit TickExecutionEngine(TickExecConfig cfg = {}) : cfg_(cfg) {}

    // Process a bar against a list of pending orders.
    // Returns vector of fills; modifies order statuses in-place.
    std::vector<Fill> process_bar(const Bar& bar,
                                   std::vector<Order>& pending) {
        std::vector<Fill> fills;
        if (pending.empty()) return fills;

        SynthConfig sc;
        sc.n_ticks   = cfg_.ticks_per_bar;
        sc.spread_bps= cfg_.spread_bps;
        auto ticks   = IntraBarSynthesizer::synthesize(bar, sc);

        SimulatedOrderBook book;
        bool first_tick = true;

        for (auto& tick : ticks) {
            book.update(tick);

            for (auto& o : pending) {
                if (!o.is_live() || o.instrument != bar.instrument) continue;

                bool triggered = false;
                double fill_px = 0;

                switch (o.type) {
                case OrderType::Market:
                    if (first_tick) {
                        auto est = book.estimate_market_fill(o.side, o.quantity);
                        fill_px   = est.avg_price;
                        triggered = true;
                    }
                    break;
                case OrderType::Limit:
                    if ((o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                        && tick.ask <= o.limit_price) {
                        fill_px = tick.ask; triggered = true;
                    } else if ((o.side == OrderSide::Sell || o.side == OrderSide::SellShort)
                        && tick.bid >= o.limit_price) {
                        fill_px = tick.bid; triggered = true;
                    }
                    break;
                case OrderType::Stop:
                    if ((o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                        && tick.price >= o.stop_price) {
                        auto est = book.estimate_market_fill(o.side, o.quantity);
                        fill_px = est.avg_price; triggered = true;
                    } else if ((o.side == OrderSide::Sell || o.side == OrderSide::SellShort)
                        && tick.price <= o.stop_price) {
                        auto est = book.estimate_market_fill(o.side, o.quantity);
                        fill_px = est.avg_price; triggered = true;
                    }
                    break;
                default: break;
                }

                if (triggered) {
                    Fill f;
                    f.order_id   = o.id;
                    f.instrument = o.instrument;
                    f.strategy_id= o.strategy_id;
                    f.side       = o.side;
                    f.quantity   = o.quantity;
                    f.price      = fill_px;
                    f.commission = o.quantity * cfg_.commission_per_share;
                    f.slippage   = std::abs(fill_px - tick.mid());
                    f.ts         = tick.ts;
                    o.status     = OrderStatus::Filled;
                    o.filled_qty = o.quantity;
                    o.avg_fill_price = fill_px;
                    o.ts_filled  = tick.ts;
                    fills.push_back(f);
                }
            }
            first_tick = false;
        }
        return fills;
    }

private:
    TickExecConfig cfg_;
};

} // namespace ql
