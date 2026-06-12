#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/regimes/RegimeEngine.h
// Institutional multi-state market regime detection engine.
//
// 9 regime states:
//   Bull, Bear, Range, VolExpansion, VolCompression,
//   RiskOn, RiskOff, LiquidityCrisis, MacroShock
//
// Detection methods (combined into a probability vector):
//   1. Volatility clustering (GARCH-like realized vol ratios)
//   2. Trend state (price vs rolling MAs, ADX-proxy)
//   3. Market internals (breadth, new highs/lows proxy)
//   4. Cross-asset correlation shift detection
//   5. Jump/tail event detection
//
// Output: RegimeState with full probability distribution + dominant regime.
// Every strategy is expected to track per-regime performance stats.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include <array>
#include <deque>
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <optional>

namespace ql {
namespace regimes {

// ── 9-state regime taxonomy ────────────────────────────────────────────────
enum class Regime : std::uint8_t {
    Bull             = 0,   // trending up, low vol, risk-on
    Bear             = 1,   // trending down, high vol, risk-off
    Range            = 2,   // mean-reverting, vol compressed
    VolExpansion     = 3,   // vol rising, direction uncertain
    VolCompression   = 4,   // vol falling (pre-breakout)
    RiskOn           = 5,   // credit spreads tight, cyclicals leading
    RiskOff          = 6,   // flight to quality, defensive rotation
    LiquidityCrisis  = 7,   // vol spike + correlation spike + spread widening
    MacroShock       = 8,   // large discrete price jumps, macro surprise
    Unknown          = 9,
};

inline const char* regime_name(Regime r) {
    switch(r) {
    case Regime::Bull:            return "BULL";
    case Regime::Bear:            return "BEAR";
    case Regime::Range:           return "RANGE";
    case Regime::VolExpansion:    return "VOL_EXPANSION";
    case Regime::VolCompression:  return "VOL_COMPRESSION";
    case Regime::RiskOn:          return "RISK_ON";
    case Regime::RiskOff:         return "RISK_OFF";
    case Regime::LiquidityCrisis: return "LIQUIDITY_CRISIS";
    case Regime::MacroShock:      return "MACRO_SHOCK";
    default:                      return "UNKNOWN";
    }
}

// ── Probability distribution over all 9 regimes ───────────────────────────
struct RegimeProbVec {
    std::array<double, 9> p = {};

    double operator[](Regime r) const { return p[static_cast<int>(r)]; }
    double& operator[](Regime r)       { return p[static_cast<int>(r)]; }

    Regime dominant() const {
        return static_cast<Regime>(
            std::max_element(p.begin(), p.end()) - p.begin());
    }

    void normalize() {
        double s = std::accumulate(p.begin(), p.end(), 0.0);
        if (s > 1e-12) for (auto& x : p) x /= s;
        else std::fill(p.begin(), p.end(), 1.0/9.0);
    }
};

// ── Regime state snapshot ─────────────────────────────────────────────────
struct RegimeState {
    Timestamp      ts           = 0;
    RegimeProbVec  probs;
    Regime         dominant     = Regime::Unknown;
    double         confidence   = 0.0;   // P(dominant) vs next best
    double         transition_p = 0.0;   // probability regime is about to change

    // Key signals
    double vol_short       = 0;   // short realized vol (annualised)
    double vol_long        = 0;   // long realized vol (annualised)
    double vol_ratio       = 0;   // vol_short / vol_long
    double trend_strength  = 0;   // signed trend: +1 strong bull, -1 strong bear
    double breadth_score   = 0;   // cross-asset breadth (-1 to +1)
    double jump_score      = 0;   // normalised jump intensity
    double correlation_spike = 0; // cross-asset correlation vs baseline
};

// ── Per-regime performance tracker ────────────────────────────────────────
struct RegimePerformance {
    Regime regime;
    int    n_bars         = 0;
    double total_return   = 0;
    double sharpe         = 0;
    double max_drawdown   = 0;
    double avg_vol        = 0;
    double win_rate       = 0;
};

// ── Engine configuration ───────────────────────────────────────────────────
struct RegimeEngineConfig {
    int    short_vol_w    = 10;
    int    long_vol_w     = 60;
    int    trend_ma_fast  = 20;
    int    trend_ma_slow  = 100;
    int    adx_period     = 14;
    int    jump_window    = 5;
    double jump_z_thresh  = 3.0;   // z-score for jump detection
    double vol_expansion_ratio = 1.5;
    double vol_compression_ratio = 0.7;
    double crisis_vol_thresh = 0.40;  // annualised vol > 40% = crisis candidate
    double crisis_corr_thresh = 0.80; // cross-asset corr > 0.80 = crisis signal
    double smoothing      = 0.80;     // exponential smoothing on probs
    int    bars_per_year  = 252;
};

// ── Multi-asset regime engine ──────────────────────────────────────────────
class RegimeEngine {
public:
    explicit RegimeEngine(RegimeEngineConfig cfg = RegimeEngineConfig())
        : cfg_(cfg) {}

    // Update with a new bar for one asset in the universe
    // Call for every asset, then call snapshot() once per timestamp
    void feed(const Bar& bar) {
        auto& s = asset_state_[bar.instrument];
        s.closes.push_back(bar.close);
        s.ranges.push_back(bar.range());
        if (s.closes.size() > 1) {
            double prev = s.closes[s.closes.size()-2];
            if (prev > 0) s.log_rets.push_back(std::log(bar.close/prev));
        }
        // Trim
        int closes_keep = std::max(cfg_.trend_ma_slow, cfg_.long_vol_w) + 10;
        while ((int)s.closes.size()   > closes_keep)          s.closes.pop_front();
        while ((int)s.ranges.size()   > cfg_.long_vol_w + 20) s.ranges.pop_front();
        while ((int)s.log_rets.size() > cfg_.long_vol_w + 20) s.log_rets.pop_front();
    }

    // Compute regime state from current universe snapshot
    RegimeState snapshot(Timestamp ts) {
        RegimeState state;
        state.ts = ts;

        // 1. Aggregate volatility across assets
        double agg_vol_s = 0, agg_vol_l = 0;
        int cnt = 0;
        for (auto& [id, s] : asset_state_) {
            double vs = rolling_vol(s.log_rets, cfg_.short_vol_w) * std::sqrt(cfg_.bars_per_year);
            double vl = rolling_vol(s.log_rets, cfg_.long_vol_w)  * std::sqrt(cfg_.bars_per_year);
            if (vs > 0 && vl > 0) { agg_vol_s += vs; agg_vol_l += vl; ++cnt; }
        }
        if (cnt > 0) { agg_vol_s /= cnt; agg_vol_l /= cnt; }
        state.vol_short = agg_vol_s;
        state.vol_long  = agg_vol_l;
        state.vol_ratio = (agg_vol_l > 0) ? agg_vol_s/agg_vol_l : 1.0;

        // 2. Trend strength: weighted average across assets
        double trend_sum = 0; int t_cnt = 0;
        for (auto& [id, s] : asset_state_) {
            double ts_val = compute_trend(s);
            if (std::isfinite(ts_val)) { trend_sum += ts_val; ++t_cnt; }
        }
        state.trend_strength = (t_cnt > 0) ? trend_sum/t_cnt : 0;

        // 3. Jump detection: max absolute z-score across assets
        double max_jump = 0;
        for (auto& [id, s] : asset_state_) {
            if ((int)s.log_rets.size() < cfg_.jump_window+1) continue;
            double mu = rolling_mean(s.log_rets, cfg_.long_vol_w);
            double sd = rolling_vol(s.log_rets, cfg_.long_vol_w);
            if (sd < 1e-9) continue;
            double z = std::abs(s.log_rets.back() - mu) / sd;
            max_jump = std::max(max_jump, z);
        }
        state.jump_score = max_jump;

        // 4. Cross-asset correlation spike
        state.correlation_spike = compute_corr_spike();

        // 5. Breadth: fraction of assets in uptrend
        double up = 0, total_a = 0;
        for (auto& [id, s] : asset_state_) {
            if (s.closes.size() < 2) continue;
            double ret = s.log_rets.empty() ? 0 : s.log_rets.back();
            up    += (ret > 0) ? 1 : 0;
            ++total_a;
        }
        state.breadth_score = (total_a > 0) ? (2.0*up/total_a - 1.0) : 0;

        // ── Regime probability assignment ──────────────────────────────────
        RegimeProbVec& p = state.probs;
        std::fill(p.p.begin(), p.p.end(), 0.0);

        double vol_r = state.vol_ratio;
        double trend = state.trend_strength;
        double jump  = state.jump_score;
        double corr  = state.correlation_spike;
        double bread = state.breadth_score;
        double vol_s = state.vol_short;

        // MacroShock: large jump + sudden vol spike
        p[Regime::MacroShock] = sigmoid((jump - cfg_.jump_z_thresh + 1) * 2.0)
                               * sigmoid((vol_r - 2.0) * 3.0);

        // LiquidityCrisis: extreme vol + correlation spike
        p[Regime::LiquidityCrisis] = sigmoid((vol_s/0.40 - 1.0) * 4.0)
                                    * sigmoid((corr - cfg_.crisis_corr_thresh) * 10.0);

        // Bear: down trend + elevated vol + negative breadth
        p[Regime::Bear] = sigmoid((-trend - 0.3) * 5.0)
                        * sigmoid((vol_r - 1.2) * 3.0)
                        * sigmoid((-bread - 0.2) * 5.0);

        // Bull: up trend + stable/falling vol + positive breadth
        p[Regime::Bull] = sigmoid((trend - 0.3) * 5.0)
                        * sigmoid((1.2 - vol_r) * 3.0)
                        * sigmoid((bread - 0.2) * 5.0);

        // Range: near-zero trend + compressed vol
        p[Regime::Range] = sigmoid((0.15 - std::abs(trend)) * 10.0)
                         * sigmoid((1.0 - vol_r) * 5.0);

        // VolExpansion: vol accelerating
        p[Regime::VolExpansion] = sigmoid((vol_r - cfg_.vol_expansion_ratio) * 4.0)
                                 * (1.0 - p.p[static_cast<int>(Regime::LiquidityCrisis)]);

        // VolCompression: vol decelerating
        p[Regime::VolCompression] = sigmoid((cfg_.vol_compression_ratio - vol_r) * 6.0)
                                   * sigmoid((0.20 - vol_s) * 10.0);

        // RiskOn: bull breadth + positive trend + low vol
        p[Regime::RiskOn] = sigmoid((bread - 0.30) * 6.0)
                           * sigmoid((0.20 - vol_s) * 8.0);

        // RiskOff: negative breadth + rising vol
        p[Regime::RiskOff] = sigmoid((-bread - 0.20) * 6.0)
                            * sigmoid((vol_r - 1.3) * 4.0);

        p.normalize();

        // Smooth with previous
        if (prev_probs_.has_value()) {
            for (int i = 0; i < 9; ++i)
                p.p[i] = cfg_.smoothing * prev_probs_->p[i]
                        + (1-cfg_.smoothing) * p.p[i];
            p.normalize();
        }
        prev_probs_ = p;

        state.dominant   = p.dominant();
        // Confidence: dominant prob minus second-best
        auto sorted = p.p;
        std::sort(sorted.rbegin(), sorted.rend());
        state.confidence = sorted[0] - sorted[1];

        // Transition probability: sum of non-dominant probabilities > 0.3
        double dom_p = p[state.dominant];
        state.transition_p = 1.0 - dom_p;

        last_ = state;
        history_.push_back(state);
        if (history_.size() > 2000) history_.pop_front();

        return state;
    }

    // Per-bar performance tracking per regime
    void record_bar_return(double ret, Regime regime) {
        auto& rp = perf_[static_cast<int>(regime)];
        rp.regime = regime;
        ++rp.n_bars;
        rp.total_return += ret;
        rp.win_rate = (rp.win_rate * (rp.n_bars-1) + (ret>0?1.0:0.0)) / rp.n_bars;
    }

    // How many bars was the system in each regime?
    std::array<int, 9> regime_bar_counts() const {
        std::array<int,9> counts = {};
        for (auto& s : history_) ++counts[static_cast<int>(s.dominant)];
        return counts;
    }

    const RegimeState&                  last()    const { return last_; }
    const std::deque<RegimeState>&      history() const { return history_; }
    const std::array<RegimePerformance,9>& perf() const { return perf_; }

    // Summarise regime distribution over recent N bars
    RegimeProbVec recent_distribution(int n_bars = 60) const {
        RegimeProbVec dist;
        int start = std::max(0, (int)history_.size()-(int)n_bars);
        for (int i = start; i < (int)history_.size(); ++i)
            dist.p[static_cast<int>(history_[i].dominant)] += 1.0;
        dist.normalize();
        return dist;
    }

private:
    struct AssetState {
        std::deque<double> closes, log_rets, ranges;
    };

    double rolling_vol(const std::deque<double>& v, int w) const {
        int n = std::min((int)v.size(), w);
        if (n < 2) return 0;
        double m = 0;
        for (int i = (int)v.size()-n; i < (int)v.size(); ++i) m += v[i];
        m /= n;
        double var = 0;
        for (int i = (int)v.size()-n; i < (int)v.size(); ++i) var += (v[i]-m)*(v[i]-m);
        return std::sqrt(var/(n-1));
    }

    double rolling_mean(const std::deque<double>& v, int w) const {
        int n = std::min((int)v.size(), w);
        if (n == 0) return 0;
        double s = 0;
        for (int i = (int)v.size()-n; i < (int)v.size(); ++i) s += v[i];
        return s/n;
    }

    double compute_trend(const AssetState& s) const {
        int nf = std::min((int)s.closes.size(), cfg_.trend_ma_fast);
        int nl = std::min((int)s.closes.size(), cfg_.trend_ma_slow);
        if (nf < cfg_.trend_ma_fast || nl < cfg_.trend_ma_slow) return 0;
        double mf = 0, ml = 0;
        for (int i = (int)s.closes.size()-nf; i < (int)s.closes.size(); ++i) mf += s.closes[i];
        for (int i = (int)s.closes.size()-nl; i < (int)s.closes.size(); ++i) ml += s.closes[i];
        mf /= nf; ml /= nl;
        double close = s.closes.back();
        // Normalised: (close/MA_fast - 1) weighted towards fast MA signal
        double fast_sig = (ml > 0) ? (close/ml - 1.0) : 0;
        double cross_sig= (ml > 0) ? (mf/ml - 1.0) * 2.0 : 0;
        return std::tanh((fast_sig + cross_sig) * 5.0);
    }

    double compute_corr_spike() const {
        // Simplified: std deviation of recent per-asset returns
        // High dispersion = low correlation, low dispersion = high correlation
        std::vector<double> last_rets;
        for (auto& [id, s] : asset_state_)
            if (!s.log_rets.empty()) last_rets.push_back(s.log_rets.back());
        if (last_rets.size() < 3) return 0;
        double m = 0; for (double r:last_rets) m+=r; m/=last_rets.size();
        double var=0; for (double r:last_rets) var+=(r-m)*(r-m); var/=last_rets.size();
        // Low variance of returns across assets = high correlation = risk-off
        // Normalise against historical: simplified as inverse std
        double std_val = std::sqrt(var);
        return std::exp(-std_val * 50.0);  // approaches 1 when all moving together
    }

    static double sigmoid(double x) { return 1.0/(1.0+std::exp(-x)); }

    RegimeEngineConfig cfg_;
    std::unordered_map<InstrumentID, AssetState> asset_state_;
    std::optional<RegimeProbVec> prev_probs_;
    RegimeState last_;
    std::deque<RegimeState> history_;
    std::array<RegimePerformance, 9> perf_ = {};
};

// ── Per-strategy regime performance tracker ────────────────────────────────
class StrategyRegimeTracker {
public:
    void record(double bar_return, const RegimeState& regime) {
        Regime r = regime.dominant;
        auto& p  = perf_[static_cast<int>(r)];
        p.regime = r;
        ++p.n_bars;
        p.total_return += bar_return;
        bar_return_history_[static_cast<int>(r)].push_back(bar_return);
    }

    const RegimePerformance& perf(Regime r) const {
        return perf_[static_cast<int>(r)];
    }

    double sharpe_in_regime(Regime r, int bars_per_year=252) const {
        const auto& rets = bar_return_history_[static_cast<int>(r)];
        if ((int)rets.size() < 5) return 0;
        double m=0; for(double x:rets) m+=x; m/=rets.size();
        double v=0; for(double x:rets) v+=(x-m)*(x-m); v/=rets.size()-1;
        return (v>0) ? m/std::sqrt(v)*std::sqrt(bars_per_year) : 0;
    }

    void print_summary() const {
        std::printf("  %-20s  %6s  %8s  %8s\n","Regime","Bars","TotRet%","Sharpe");
        for (int i=0; i<9; ++i) {
            if (perf_[i].n_bars==0) continue;
            std::printf("  %-20s  %6d  %8.2f%%  %8.3f\n",
                regime_name(static_cast<Regime>(i)),
                perf_[i].n_bars,
                perf_[i].total_return*100,
                sharpe_in_regime(static_cast<Regime>(i)));
        }
    }

private:
    std::array<RegimePerformance,9>      perf_    = {};
    std::array<std::vector<double>,9>    bar_return_history_;
};

} // namespace regimes
} // namespace ql
