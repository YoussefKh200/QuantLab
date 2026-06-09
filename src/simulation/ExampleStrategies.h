#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/ExampleStrategies.h
// Production-grade strategy implementations.
//
// 1. MACrossStrategy       — MA crossover (single or multi-asset)
// 2. CrossSectionalFactor  — Long/short factor portfolio (institutional style)
// 3. VolTargetStrategy     — Volatility-targeted trend following
// 4. MeanReversionStrategy — Z-score mean reversion with bands
// ═══════════════════════════════════════════════════════════════════════════
#include "Backtester.h"
#include "../factors/AlphaEngine.h"
#include "../portfolio/PortfolioConstruction.h"
#include "../risk/RiskEngine.h"
#include <deque>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace ql {

// ── Rolling window helpers ─────────────────────────────────────────────────
class RollingMean {
public:
    explicit RollingMean(int p) : p_(p) {}
    void push(double v) {
        q_.push_back(v); sum_ += v;
        if ((int)q_.size() > p_) { sum_ -= q_.front(); q_.pop_front(); }
    }
    bool   ready() const { return (int)q_.size() == p_; }
    double value() const { return q_.empty() ? 0 : sum_/q_.size(); }
    void   reset(int p)  { p_=p; q_.clear(); sum_=0; }
private:
    int p_; double sum_=0; std::deque<double> q_;
};

class RollingVol {
public:
    explicit RollingVol(int p, int bpy=252) : p_(p), bpy_(bpy) {}
    void push(double ret) {
        q_.push_back(ret);
        if ((int)q_.size() > p_) q_.pop_front();
    }
    bool ready() const { return (int)q_.size() == p_; }
    double value() const {
        if (q_.size() < 2) return 0;
        double m=0; for (double x:q_) m+=x; m/=q_.size();
        double v=0; for (double x:q_) v+=(x-m)*(x-m);
        return std::sqrt(v/(q_.size()-1)*bpy_);
    }
private:
    int p_, bpy_; std::deque<double> q_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 1: Moving Average Crossover
// ═══════════════════════════════════════════════════════════════════════════
struct MACrossParams {
    int    fast            = 10;
    int    slow            = 50;
    double notional        = 100'000.0;
    bool   allow_short     = false;
    bool   size_by_vol     = false;   // vol-target position sizing
    double target_vol      = 0.10;
    int    vol_window      = 20;
};

class MACrossStrategy : public IStrategy {
public:
    explicit MACrossStrategy(MACrossParams p = {}) : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s = sym_[bar.instrument];
        if (!s.init) {
            s.fast_ma  = RollingMean(p_.fast);
            s.slow_ma  = RollingMean(p_.slow);
            s.vol_calc = RollingVol(p_.vol_window);
            s.init     = true;
        }

        s.fast_ma.push(bar.close);
        s.slow_ma.push(bar.close);
        if (s.prev_close > 0)
            s.vol_calc.push(std::log(bar.close/s.prev_close));
        s.prev_close = bar.close;

        if (!s.fast_ma.ready() || !s.slow_ma.ready()) return;

        double fn = s.fast_ma.value(), sn = s.slow_ma.value();
        bool cross_up = fn > sn && s.pf <= s.ps;
        bool cross_dn = fn <=sn && s.pf >  s.ps;
        s.pf = fn; s.ps = sn;

        // Size
        double notional = p_.notional;
        if (p_.size_by_vol && s.vol_calc.ready() && s.vol_calc.value() > 0)
            notional *= p_.target_vol / s.vol_calc.value();
        double qty = std::floor(notional / bar.close);
        if (qty < 1) return;

        double cur_qty = position_qty(bar.instrument);

        if (cross_up && cur_qty <= 0) {
            if (cur_qty < 0) buy_market(bar.instrument, -cur_qty);
            buy_market(bar.instrument, qty);
            s.qty = qty;
        } else if (cross_dn && cur_qty > 0) {
            sell_market(bar.instrument, cur_qty);
            s.qty = 0;
            if (p_.allow_short && qty > 0) {
                sell_market(bar.instrument, qty);
                s.qty = -qty;
            }
        }
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto& [inst, s] : sym_) {
            double q = position_qty(inst);
            if (q > 0) sell_market(inst, q);
            else if (q < 0) buy_market(inst, -q);
        }
    }

private:
    struct SymState {
        RollingMean fast_ma{10}, slow_ma{50};
        RollingVol  vol_calc{20};
        double pf=0, ps=0, prev_close=0, qty=0;
        bool init=false;
    };
    MACrossParams p_;
    std::unordered_map<InstrumentID, SymState> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 2: Cross-Sectional Factor Portfolio
// Long top quintile, short bottom quintile, risk-parity weighted
// ═══════════════════════════════════════════════════════════════════════════
struct CrossSectionalParams {
    int    rebal_freq      = 21;
    int    lookback        = 252;
    double long_pct        = 0.20;
    double short_pct       = 0.20;
    double gross_notional  = 1000000.0;
    bool   dollar_neutral  = true;
};

class CrossSectionalStrategy : public IStrategy {
public:
    // Custom factor function
    using FactorFn = std::function<double(InstrumentID, const std::deque<Bar>&)>;

    CrossSectionalParams p_;
    FactorFn             factor_fn_;

    CrossSectionalStrategy(CrossSectionalParams p, FactorFn fn)
        : p_(p), factor_fn_(std::move(fn)) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        // Buffer bars per instrument
        auto& buf = bar_buf_[bar.instrument];
        buf.push_back(bar);
        if ((int)buf.size() > p_.lookback + 10) buf.pop_front();

        ++bar_count_;
        if (bar_count_ % p_.rebal_freq != 0) return;

        // Compute factor scores
        std::vector<std::pair<InstrumentID, double>> scores;
        for (auto& [inst, bars] : bar_buf_) {
            if ((int)bars.size() < p_.lookback / 2) continue;
            double s = factor_fn_(inst, bars);
            if (std::isfinite(s)) scores.push_back({inst, s});
        }
        if (scores.size() < 10) return;

        // Rank
        std::sort(scores.begin(), scores.end(),
                  [](auto& a, auto& b){ return a.second > b.second; });
        int n     = (int)scores.size();
        int n_top = std::max(1, (int)(n * p_.long_pct));
        int n_bot = std::max(1, (int)(n * p_.short_pct));

        // New target positions
        std::unordered_map<InstrumentID, double> targets;
        double per_long  = p_.gross_notional * 0.5 / n_top;
        double per_short = p_.gross_notional * 0.5 / n_bot;

        for (int i = 0;   i < n_top; ++i)
            targets[scores[i].first] = per_long;
        for (int i = n-n_bot; i < n; ++i)
            targets[scores[i].first] = -per_short;

        // Execute rebalance: close positions not in new target, enter new
        for (auto& [inst, tgt_notional] : targets) {
            double cur_qty = position_qty(inst);
            auto it = bar_buf_.find(inst);
            if (it == bar_buf_.end() || it->second.empty()) continue;
            double px = it->second.back().close;
            if (px <= 0) continue;

            double tgt_qty = std::floor(std::abs(tgt_notional) / px)
                           * (tgt_notional > 0 ? 1 : -1);
            double delta   = tgt_qty - cur_qty;

            if (std::abs(delta) < 1) continue;
            if (delta > 0) buy_market(inst, delta);
            else           sell_market(inst, -delta);
        }

        // Close positions not in new target universe
        for (auto& [inst, pos] : pf.all_positions()) {
            if (pos.is_flat()) continue;
            if (targets.find(inst) == targets.end()) {
                if (pos.quantity > 0) sell_market(inst, pos.quantity);
                else                  buy_market(inst, -pos.quantity);
            }
        }
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto& [inst, pos] : pf.all_positions()) {
            if (pos.is_flat()) continue;
            if (pos.quantity > 0) sell_market(inst, pos.quantity);
            else                  buy_market(inst, -pos.quantity);
        }
    }

private:
    std::unordered_map<InstrumentID, std::deque<Bar>> bar_buf_;
    int bar_count_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 3: Volatility-Targeted Trend Following
// Scales position size by inverse realized volatility
// ═══════════════════════════════════════════════════════════════════════════
struct VolTargetParams {
    int    fast           = 20;
    int    slow           = 100;
    double target_vol     = 0.10;     // 10% annual target
    int    vol_window     = 60;
    double max_leverage   = 2.0;
    double notional       = 900'000.0;
};

class VolTargetTrendStrategy : public IStrategy {
public:
    explicit VolTargetTrendStrategy(VolTargetParams p = {}) : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s = sym_[bar.instrument];
        s.fast_ma.push(bar.close);
        s.slow_ma.push(bar.close);
        if (s.prev > 0) s.vol.push(std::log(bar.close/s.prev));
        s.prev = bar.close;

        if (!s.fast_ma.ready() || !s.slow_ma.ready() || !s.vol.ready()) return;

        double trend = (s.fast_ma.value() / s.slow_ma.value()) - 1.0;
        double signal = std::tanh(trend * 20);  // smooth sigmoid
        double rv = s.vol.value();
        double scale = (rv > 0) ? std::min(p_.target_vol/rv, p_.max_leverage) : 0;
        double target_notional = p_.notional * signal * scale;
        double target_qty = (bar.close > 0)
            ? std::round(target_notional / bar.close) : 0;

        double cur = position_qty(bar.instrument);
        double delta = target_qty - cur;
        if (std::abs(delta) < 1) return;
        if (delta > 0) buy_market(bar.instrument, delta);
        else           sell_market(bar.instrument, -delta);
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto& [inst, s] : sym_) {
            double q = position_qty(inst);
            if (q > 0) sell_market(inst, q);
            else if (q < 0) buy_market(inst, -q);
        }
    }

private:
    struct Sym {
        RollingMean fast_ma{20}, slow_ma{100};
        RollingVol  vol{60};
        double      prev=0;
    };
    VolTargetParams p_;
    std::unordered_map<InstrumentID, Sym> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 4: Z-Score Mean Reversion with Bollinger Bands
// ═══════════════════════════════════════════════════════════════════════════
struct MeanRevParams {
    int    lookback       = 20;
    double entry_z        = -2.0;
    double exit_z         = 0.0;
    double stop_z         = -4.0;   // emergency stop
    double notional       = 80'000.0;
};

class MeanReversionStrategy : public IStrategy {
public:
    explicit MeanReversionStrategy(MeanRevParams p = {}) : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s = sym_[bar.instrument];
        s.mu_calc.push(bar.close);
        s.std_buf.push_back(bar.close);
        if ((int)s.std_buf.size() > p_.lookback) s.std_buf.pop_front();
        if (!s.mu_calc.ready() || (int)s.std_buf.size() < p_.lookback) return;

        double mu = s.mu_calc.value();
        double var = 0;
        for (double x : s.std_buf) var += (x-mu)*(x-mu);
        double sd = std::sqrt(var/(s.std_buf.size()-1));
        if (sd < 1e-9) return;

        double z   = (bar.close - mu) / sd;
        double qty = position_qty(bar.instrument);

        if (qty == 0 && z <= p_.entry_z) {
            double q = std::floor(p_.notional / bar.close);
            if (q > 0) { buy_market(bar.instrument, q); s.qty = q; }
        } else if (qty > 0 && z >= p_.exit_z) {
            sell_market(bar.instrument, qty); s.qty = 0;
        } else if (qty > 0 && z <= p_.stop_z) {
            sell_market(bar.instrument, qty); s.qty = 0;
        }
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto& [inst, s] : sym_) {
            if (s.qty > 0) { sell_market(inst, s.qty); s.qty = 0; }
        }
    }

private:
    struct Sym {
        RollingMean        mu_calc{20};
        std::deque<double> std_buf;
        double             qty=0;
    };
    MeanRevParams p_;
    std::unordered_map<InstrumentID, Sym> sym_;
};

} // namespace ql
