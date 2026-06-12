#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/RegimeDetection.h
// Volatility regime classifier and regime-aware strategy.
// Ported from QuantEngine v2, integrated into ql:: namespace.
//
// RegimeDetector: soft two-state classifier (Bull/Bear/Transition).
// Combines realized-vol ratio, trend vs 200MA, ATR/price, momentum.
// Output: bull_probability ∈ [0,1] with exponential smoothing.
//
// RegimeFilteredMAStrategy: only takes long signals when bull_prob
// exceeds threshold; force-exits on regime flip.
// ═══════════════════════════════════════════════════════════════════════════
#include "Backtester.h"
#include "ExampleStrategies.h"
#include <deque>
#include <cmath>
#include <algorithm>
#include <string>
#include <unordered_map>

namespace ql {

// ── Regime states ──────────────────────────────────────────────────────────
enum class MarketRegime : std::uint8_t { Bull, Transition, Bear };

struct RegimeSignals {
    double realized_vol_short = 0;  // short-window annualised vol
    double realized_vol_long  = 0;  // long-window annualised vol
    double vol_ratio          = 0;  // short / long
    double trend_score        = 0;  // (close / MA200) - 1
    double range_ratio        = 0;  // ATR / close
    double momentum_score     = 0;  // 12m minus 1m momentum
    double bull_probability   = 0.5;
    MarketRegime regime       = MarketRegime::Bull;
};

struct RegimeDetectorConfig {
    int    short_vol_window    = 10;
    int    long_vol_window     = 60;
    int    trend_ma_window     = 200;
    int    atr_window          = 14;
    int    mom_short_window    = 21;
    int    mom_long_window     = 252;
    double vol_ratio_threshold = 1.4;
    double smoothing           = 0.85;
    int    bars_per_year       = 252;
};

// ── Regime detector ────────────────────────────────────────────────────────
class RegimeDetector {
public:
    explicit RegimeDetector(RegimeDetectorConfig cfg = RegimeDetectorConfig())
        : cfg_(cfg) {}

    RegimeSignals update(const Bar& bar) {
        closes_.push_back(bar.close);
        ranges_.push_back(bar.range());
        if (closes_.size() > 1) {
            double prev = closes_[closes_.size()-2];
            if (prev > 0) log_rets_.push_back(std::log(bar.close/prev));
        }

        // Trim history
        while ((int)closes_.size()  > cfg_.trend_ma_window + 10) closes_.pop_front();
        while ((int)ranges_.size()  > std::max(cfg_.long_vol_window, cfg_.atr_window)+5)
            ranges_.pop_front();
        while ((int)log_rets_.size()> cfg_.long_vol_window + 5) log_rets_.pop_front();

        RegimeSignals s;

        // Realized vol (short & long)
        if ((int)log_rets_.size() >= cfg_.short_vol_window)
            s.realized_vol_short = rolling_std(log_rets_,
                log_rets_.size()-cfg_.short_vol_window, cfg_.short_vol_window)
                * std::sqrt(cfg_.bars_per_year);
        if ((int)log_rets_.size() >= cfg_.long_vol_window)
            s.realized_vol_long = rolling_std(log_rets_,
                log_rets_.size()-cfg_.long_vol_window, cfg_.long_vol_window)
                * std::sqrt(cfg_.bars_per_year);
        if (s.realized_vol_long > 1e-9)
            s.vol_ratio = s.realized_vol_short / s.realized_vol_long;

        // Trend vs 200MA
        if ((int)closes_.size() >= cfg_.trend_ma_window) {
            double ma = 0;
            for (int i = (int)closes_.size()-cfg_.trend_ma_window;
                 i < (int)closes_.size(); ++i) ma += closes_[i];
            ma /= cfg_.trend_ma_window;
            s.trend_score = (bar.close / ma) - 1.0;
        }

        // ATR ratio
        if ((int)ranges_.size() >= cfg_.atr_window) {
            double atr = 0;
            for (int i = (int)ranges_.size()-cfg_.atr_window;
                 i < (int)ranges_.size(); ++i) atr += ranges_[i];
            atr /= cfg_.atr_window;
            s.range_ratio = (bar.close > 0) ? atr/bar.close : 0;
        }

        // Momentum
        if ((int)closes_.size() >= cfg_.mom_long_window) {
            double p12m = closes_[closes_.size()-cfg_.mom_long_window];
            double p1m  = closes_[closes_.size()-std::min((int)closes_.size()-1,
                                                           cfg_.mom_short_window)];
            double m12  = (p12m > 0) ? bar.close/p12m - 1 : 0;
            double m1   = (p1m  > 0) ? bar.close/p1m  - 1 : 0;
            s.momentum_score = m12 - m1*2;  // penalise short-term reversal
        }

        // Soft bear probability
        double bear_sig = 0, weight = 0;
        if (s.vol_ratio > 0) {
            double w = 0.40;
            bear_sig += w * std::clamp((s.vol_ratio-1.0)/(cfg_.vol_ratio_threshold-1.0),0.0,1.0);
            weight   += w;
        }
        if (s.trend_score != 0) {
            double w = 0.35;
            bear_sig += w * std::clamp(-s.trend_score/0.20, 0.0, 1.0);
            weight   += w;
        }
        if (s.range_ratio > 0) {
            double w = 0.15;
            bear_sig += w * std::clamp(s.range_ratio/0.04, 0.0, 1.0);
            weight   += w;
        }
        if (s.momentum_score != 0) {
            double w = 0.10;
            bear_sig += w * std::clamp(-s.momentum_score/0.20, 0.0, 1.0);
            weight   += w;
        }
        double raw = (weight > 0) ? bear_sig/weight : 0.3;
        smoothed_  = cfg_.smoothing * smoothed_ + (1-cfg_.smoothing) * raw;

        s.bull_probability = 1.0 - smoothed_;
        s.regime = (smoothed_ > 0.65) ? MarketRegime::Bear
                 : (smoothed_ < 0.35) ? MarketRegime::Bull
                                       : MarketRegime::Transition;
        last_ = s;
        return s;
    }

    const RegimeSignals& last()    const { return last_; }
    MarketRegime regime()          const { return last_.regime; }
    double       bull_probability()const { return last_.bull_probability; }

    static std::string name(MarketRegime r) {
        switch(r) {
        case MarketRegime::Bull:       return "BULL";
        case MarketRegime::Transition: return "TRANSITION";
        case MarketRegime::Bear:       return "BEAR";
        }
        return "?";
    }

private:
    double rolling_std(const std::deque<double>& d,
                        std::size_t start, int len) const {
        if ((int)(d.size()-start) < len) return 0;
        double m = 0;
        for (int i = 0; i < len; ++i) m += d[start+i];
        m /= len;
        double v = 0;
        for (int i = 0; i < len; ++i) v += (d[start+i]-m)*(d[start+i]-m);
        return std::sqrt(v/(len-1));
    }

    RegimeDetectorConfig cfg_;
    std::deque<double>   closes_, log_rets_, ranges_;
    double               smoothed_ = 0.3;
    RegimeSignals        last_;
};

// ── Regime-filtered MA strategy ───────────────────────────────────────────
struct RFMAParams {
    int    fast_period  = 10;
    int    slow_period  = 50;
    double notional     = 100'000.0;
    bool   short_in_bear= false;
    double min_bull_prob= 0.55;
    double max_bear_prob= 0.60;
};

class RegimeFilteredMAStrategy : public IStrategy {
public:
    explicit RegimeFilteredMAStrategy(RFMAParams p = RFMAParams()) : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s = sym_[bar.instrument];
        if (!s.init) {
            s.fast = RollingMean(p_.fast_period);
            s.slow = RollingMean(p_.slow_period);
            s.init = true;
        }
        s.fast.push(bar.close);
        s.slow.push(bar.close);
        auto regime = s.detector.update(bar);

        if (!s.fast.ready() || !s.slow.ready()) return;

        double fn = s.fast.value(), sn = s.slow.value();
        bool cross_up = fn > sn && s.pf <= s.ps;
        bool cross_dn = fn <=sn && s.pf >  s.ps;
        s.pf = fn; s.ps = sn;

        double cur_qty = position_qty(bar.instrument);
        double bp      = regime.bull_probability;

        if (cross_up && bp >= p_.min_bull_prob && cur_qty <= 0) {
            if (cur_qty < 0) buy_market(bar.instrument, -cur_qty);
            double q = std::floor(p_.notional / bar.close);
            if (q > 0) { buy_market(bar.instrument, q); s.qty = q; }

        } else if (cross_dn && cur_qty > 0) {
            sell_market(bar.instrument, cur_qty); s.qty = 0;
            if (p_.short_in_bear && (1-bp) >= p_.max_bear_prob) {
                double q = std::floor(p_.notional / bar.close);
                if (q > 0) { sell_market(bar.instrument, q); s.qty = -q; }
            }

        } else if (cur_qty > 0 && bp < (1.0 - p_.max_bear_prob)) {
            // Forced regime exit — don't wait for MA cross
            sell_market(bar.instrument, cur_qty); s.qty = 0;
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
    struct Sym {
        RollingMean    fast{10}, slow{50};
        RegimeDetector detector;
        double pf=0, ps=0, qty=0;
        bool   init=false;
    };
    RFMAParams p_;
    std::unordered_map<InstrumentID, Sym> sym_;
};

} // namespace ql
