#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/PortfolioStrategy.h
// Portfolio-construction-aware multi-asset strategy.
// Ported from QuantEngine v2, wired to ql::construction:: methods.
//
// Maintains rolling return history per symbol.
// On each rebalance bar, computes target weights via the chosen
// allocation method (EqualWeight / InvVol / RiskParity / Kelly),
// then executes delta orders to reach targets.
// ═══════════════════════════════════════════════════════════════════════════
#include "Backtester.h"
#include "ExampleStrategies.h"
#include "../portfolio/PortfolioConstruction.h"
#include <deque>
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace ql {

enum class DynamicAllocMethod {
    EqualWeight, InvVol, RiskParity, Kelly, MaxSharpe, HRP
};

struct PortfolioStrategyParams {
    int    fast_period     = 10;
    int    slow_period     = 50;
    int    vol_lookback    = 60;
    int    rebal_freq      = 21;
    double total_notional  = 900'000.0;
    double max_weight      = 0.40;
    DynamicAllocMethod method = DynamicAllocMethod::InvVol;
    bool   require_signal  = true;  // only allocate to instruments with long signal
};

class PortfolioAwareStrategy : public IStrategy {
public:
    explicit PortfolioAwareStrategy(PortfolioStrategyParams p = PortfolioStrategyParams())
        : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s = sym_[bar.instrument];
        if (!s.init) {
            s.fast = RollingMean(p_.fast_period);
            s.slow = RollingMean(p_.slow_period);
            s.init = true;
        }
        s.fast.push(bar.close);
        s.slow.push(bar.close);
        s.last_close = bar.close;
        s.last_inst  = bar.instrument;

        // Track returns for covariance estimation
        if (s.prev_close > 0) {
            s.returns.push_back(std::log(bar.close / s.prev_close));
            if ((int)s.returns.size() > p_.vol_lookback + 5)
                s.returns.pop_front();
        }
        s.prev_close = bar.close;

        // Update MA signal
        if (s.fast.ready() && s.slow.ready()) {
            double fn = s.fast.value(), sn = s.slow.value();
            bool up = fn > sn && s.pf <= s.ps;
            bool dn = fn <=sn && s.pf >  s.ps;
            s.pf = fn; s.ps = sn;
            if (up) s.signal =  1;
            if (dn) s.signal = -1;
        }

        ++bar_count_;
        if (bar_count_ % p_.rebal_freq == 0)
            rebalance(sim, pf);
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto& [inst, pos] : pf.all_positions()) {
            if (pos.is_flat()) continue;
            if (pos.quantity > 0) sell_market(inst, pos.quantity);
            else                  buy_market(inst, -pos.quantity);
        }
    }

private:
    struct SymState {
        RollingMean fast{10}, slow{50};
        std::deque<double> returns;
        double pf=0, ps=0, prev_close=0, last_close=0;
        int    signal = 0;
        bool   init   = false;
        InstrumentID last_inst = 0;
    };

    void rebalance(MarketSimulator& sim, Portfolio& pf) {
        // Determine active universe
        std::vector<InstrumentID> active;
        for (auto& [inst, s] : sym_) {
            bool in = !p_.require_signal || s.signal > 0;
            if (in && s.last_close > 0) active.push_back(inst);
        }
        if (active.empty()) {
            // Close everything
            for (auto& [inst, pos] : pf.all_positions())
                if (!pos.is_flat()) {
                    if (pos.quantity > 0) sell_market(inst, pos.quantity);
                    else buy_market(inst, -pos.quantity);
                }
            return;
        }

        // Build return matrix and cov for active instruments
        int n = (int)active.size();
        std::vector<std::vector<double>> ret_matrix(n);
        for (int i = 0; i < n; ++i) {
            auto& rets = sym_[active[i]].returns;
            ret_matrix[i] = std::vector<double>(rets.begin(), rets.end());
        }
        auto cov = construction::compute_cov(ret_matrix);

        // Compute weights
        construction::WeightVec weights(n);
        switch (p_.method) {
        case DynamicAllocMethod::EqualWeight:
            weights = construction::equal_weight(n);
            break;
        case DynamicAllocMethod::InvVol:
            weights = construction::inverse_vol(cov);
            break;
        case DynamicAllocMethod::RiskParity:
            weights = construction::risk_parity(cov);
            break;
        case DynamicAllocMethod::HRP:
            weights = construction::hrp(cov);
            break;
        case DynamicAllocMethod::MaxSharpe: {
            // Use historical mean as expected return
            construction::ReturnVec mu(n);
            for (int i = 0; i < n; ++i) {
                auto& r = ret_matrix[i];
                if (!r.empty()) {
                    double m = 0; for (double x : r) m += x;
                    mu[i] = m / r.size() * 252;
                }
            }
            weights = construction::max_sharpe(cov, mu, 0.05);
            break;
        }
        case DynamicAllocMethod::Kelly: {
            construction::ReturnVec mu(n);
            for (int i = 0; i < n; ++i) {
                auto& r = ret_matrix[i];
                if (!r.empty()) {
                    double m = 0; for (double x : r) m += x;
                    mu[i] = m / r.size() * 252;
                }
            }
            weights = construction::max_sharpe(cov, mu, 0.05); // Kelly ≈ max Sharpe scaled
            break;
        }
        }

        // Clamp and normalise
        double sum_w = 0;
        for (int i = 0; i < n; ++i) {
            weights[i] = std::clamp(weights[i], 0.0, p_.max_weight);
            sum_w += weights[i];
        }
        if (sum_w > 1e-9)
            for (auto& w : weights) w /= sum_w;

        // Execute rebalance
        // First: close instruments not in active universe
        for (auto& [inst, pos] : pf.all_positions()) {
            if (pos.is_flat()) continue;
            bool in_active = false;
            for (auto a : active) if (a == inst) { in_active = true; break; }
            if (!in_active) {
                if (pos.quantity > 0) sell_market(inst, pos.quantity);
                else buy_market(inst, -pos.quantity);
            }
        }

        // Then: move active instruments to target
        for (int i = 0; i < n; ++i) {
            InstrumentID inst = active[i];
            double px = sym_[inst].last_close;
            if (px <= 0) continue;
            double target_notional = p_.total_notional * weights[i];
            double target_qty = std::floor(target_notional / px);
            double cur_qty = position_qty(inst);
            double delta = target_qty - cur_qty;
            if (std::abs(delta) < 1) continue;
            if (delta > 0) buy_market(inst, delta);
            else           sell_market(inst, -delta);
        }
    }

    PortfolioStrategyParams p_;
    std::unordered_map<InstrumentID, SymState> sym_;
    int bar_count_ = 0;
};

} // namespace ql
