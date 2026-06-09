#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/factors/AlphaEngine.h
// Alpha factor research framework.
//
// Design: Factors are functions from bar history → signal score.
// The AlphaEngine manages:
//   - Factor computation (parallel, per-instrument)
//   - Cross-sectional normalization (z-score, rank, winsorize)
//   - Information coefficient (IC) measurement
//   - Factor decay analysis
//   - Factor combination / weighting
//   - Alpha attribution
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/ThreadPool.h"
#include "../data/DataManager.h"
#include <functional>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <optional>
#include <memory>
#include <deque>
#include <span>

namespace ql {
namespace factors {

// ── Factor value: score for one instrument at one time ────────────────────
struct FactorValue {
    InstrumentID instrument;
    Timestamp    ts;
    double       raw;       // raw factor value
    double       score;     // normalized (z-score or rank)
    double       percentile;// 0-100 cross-sectional rank
};

// ── Factor result: universe snapshot ──────────────────────────────────────
using FactorResult = std::vector<FactorValue>;

// ── Factor function concept ───────────────────────────────────────────────
// Must be callable: (bars_span) → double
template<typename F>
concept FactorFn = requires(F f, std::span<const Bar> bars) {
    { f(bars) } -> std::convertible_to<double>;
};

// ── Base factor interface ──────────────────────────────────────────────────
class IFactor {
public:
    virtual ~IFactor() = default;
    virtual double compute(std::span<const Bar> bars) const = 0;
    virtual std::string name() const = 0;
    virtual int         min_bars() const = 0;
};

// ── Adapter: wrap a lambda as a factor ───────────────────────────────────
class LambdaFactor final : public IFactor {
public:
    LambdaFactor(std::string name, int min_bars,
                 std::function<double(std::span<const Bar>)> fn)
        : name_(std::move(name)), min_bars_(min_bars), fn_(std::move(fn)) {}

    double compute(std::span<const Bar> bars) const override { return fn_(bars); }
    std::string name()     const override { return name_; }
    int         min_bars() const override { return min_bars_; }

private:
    std::string name_;
    int min_bars_;
    std::function<double(std::span<const Bar>)> fn_;
};

// ═══════════════════════════════════════════════════════════════════════════
// CANONICAL FACTORS
// ═══════════════════════════════════════════════════════════════════════════

// ── Momentum (price return over lookback) ─────────────────────────────────
inline std::shared_ptr<IFactor> momentum(int lookback) {
    return std::make_shared<LambdaFactor>(
        "Momentum(" + std::to_string(lookback) + ")",
        lookback + 1,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback + 1) return 0;
            double p0 = bars[bars.size()-1-lookback].close;
            double p1 = bars.back().close;
            return (p0 > 0) ? std::log(p1/p0) : 0;
        });
}

// ── Momentum skip (12-1 month momentum — standard in literature) ──────────
inline std::shared_ptr<IFactor> momentum_skip(int lookback = 252, int skip = 21) {
    return std::make_shared<LambdaFactor>(
        "MomentumSkip(" + std::to_string(lookback) + "," + std::to_string(skip) + ")",
        lookback + 1,
        [lookback, skip](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback + 1) return 0;
            double p0 = bars[bars.size()-1-lookback].close;
            double p1 = bars[bars.size()-1-skip].close;
            return (p0 > 0) ? std::log(p1/p0) : 0;
        });
}

// ── Reversal (short-term mean reversion) ─────────────────────────────────
inline std::shared_ptr<IFactor> reversal(int lookback = 5) {
    return std::make_shared<LambdaFactor>(
        "Reversal(" + std::to_string(lookback) + ")",
        lookback + 1,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback + 1) return 0;
            double ret = std::log(bars.back().close / bars[bars.size()-1-lookback].close);
            return -ret;  // reversal = negative recent return
        });
}

// ── Realized volatility ───────────────────────────────────────────────────
inline std::shared_ptr<IFactor> realized_vol(int lookback = 20) {
    return std::make_shared<LambdaFactor>(
        "RealizedVol(" + std::to_string(lookback) + ")",
        lookback + 1,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback + 1) return 0;
            double var = 0;
            int n = (int)bars.size();
            for (int i = n-lookback; i < n; ++i) {
                double r = std::log(bars[i].close/bars[i-1].close);
                var += r*r;
            }
            return std::sqrt(var / lookback * 252.0);
        });
}

// ── Idiosyncratic vol (vol orthogonal to market) ──────────────────────────
// Note: requires market returns — simplified here as vol rank factor
inline std::shared_ptr<IFactor> low_vol(int lookback = 60) {
    return std::make_shared<LambdaFactor>(
        "LowVol(" + std::to_string(lookback) + ")",
        lookback + 1,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback + 1) return 0;
            double var = 0;
            int n = (int)bars.size();
            for (int i = n-lookback; i < n; ++i) {
                double r = std::log(bars[i].close/bars[i-1].close);
                var += r*r;
            }
            double vol = std::sqrt(var / lookback * 252.0);
            return -vol;  // LOW vol is the signal (betting against vol)
        });
}

// ── Volume trend ──────────────────────────────────────────────────────────
inline std::shared_ptr<IFactor> volume_trend(int short_w = 5, int long_w = 20) {
    return std::make_shared<LambdaFactor>(
        "VolumeTrend",
        long_w + 1,
        [short_w, long_w](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < long_w + 1) return 0;
            double sv = 0, lv = 0;
            int n = (int)bars.size();
            for (int i = n-short_w; i < n; ++i) sv += bars[i].volume;
            for (int i = n-long_w; i < n; ++i)  lv += bars[i].volume;
            sv /= short_w; lv /= long_w;
            return (lv > 0) ? sv/lv - 1 : 0;
        });
}

// ── Price-to-52-week-high (breakout proximity) ────────────────────────────
inline std::shared_ptr<IFactor> nearness_to_high(int lookback = 252) {
    return std::make_shared<LambdaFactor>(
        "Nearness52wHigh",
        lookback,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback) return 0;
            int n = (int)bars.size();
            double high52 = 0;
            for (int i = n-lookback; i < n; ++i)
                high52 = std::max(high52, bars[i].high);
            return (high52 > 0) ? bars.back().close / high52 : 0;
        });
}

// ── Seasonality: calendar month return (historically) ─────────────────────
inline std::shared_ptr<IFactor> monthly_seasonality() {
    return std::make_shared<LambdaFactor>(
        "Seasonality",
        22,
        [](std::span<const Bar> bars) -> double {
            if (bars.empty()) return 0;
            // Return the 1-month lagged return (next month expected to repeat)
            int n = (int)bars.size();
            if (n < 22) return 0;
            return std::log(bars[n-1].close / bars[n-22].close);
        });
}

// ── VWAP deviation ────────────────────────────────────────────────────────
inline std::shared_ptr<IFactor> vwap_deviation(int lookback = 10) {
    return std::make_shared<LambdaFactor>(
        "VWAPDev(" + std::to_string(lookback) + ")",
        lookback,
        [lookback](std::span<const Bar> bars) -> double {
            if ((int)bars.size() < lookback) return 0;
            double sum_vwap = 0, sum_vol = 0;
            int n = (int)bars.size();
            for (int i = n-lookback; i < n; ++i) {
                sum_vwap += bars[i].vwap * bars[i].volume;
                sum_vol  += bars[i].volume;
            }
            double avg_vwap = (sum_vol > 0) ? sum_vwap/sum_vol : bars.back().close;
            return (avg_vwap > 0) ? (bars.back().close - avg_vwap) / avg_vwap : 0;
        });
}

// ═══════════════════════════════════════════════════════════════════════════
// CROSS-SECTIONAL NORMALIZATION
// ═══════════════════════════════════════════════════════════════════════════

inline void winsorize(FactorResult& fr, double z_limit = 3.0) {
    if (fr.empty()) return;
    double mean = 0, var = 0;
    for (auto& fv : fr) mean += fv.raw;
    mean /= fr.size();
    for (auto& fv : fr) var += (fv.raw-mean)*(fv.raw-mean);
    double sd = std::sqrt(var/fr.size());
    if (sd < 1e-9) return;
    double lo = mean - z_limit*sd, hi = mean + z_limit*sd;
    for (auto& fv : fr) fv.raw = std::clamp(fv.raw, lo, hi);
}

inline void zscore_normalize(FactorResult& fr) {
    if (fr.empty()) return;
    double mean = 0, var = 0;
    for (auto& fv : fr) mean += fv.raw;
    mean /= fr.size();
    for (auto& fv : fr) var += (fv.raw-mean)*(fv.raw-mean);
    double sd = std::sqrt(var/fr.size());
    if (sd < 1e-9) return;
    for (auto& fv : fr) fv.score = (fv.raw - mean) / sd;
}

inline void rank_normalize(FactorResult& fr) {
    if (fr.empty()) return;
    std::vector<std::size_t> idx(fr.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [&fr](std::size_t a, std::size_t b){ return fr[a].raw < fr[b].raw; });
    double n = (double)fr.size();
    for (std::size_t i = 0; i < fr.size(); ++i) {
        fr[idx[i]].score       = (i + 0.5) / n;  // [0, 1]
        fr[idx[i]].percentile  = (i + 1) * 100.0 / n;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// IC ANALYSIS
// ═══════════════════════════════════════════════════════════════════════════

// Spearman rank IC between factor scores and forward returns
inline double spearman_ic(const std::vector<double>& scores,
                            const std::vector<double>& fwd_rets) {
    if (scores.size() != fwd_rets.size() || scores.empty()) return 0;
    int n = (int)scores.size();

    auto rank_of = [n](std::vector<double> v) {
        std::vector<int> idx(n);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&v](int a, int b){ return v[a]<v[b]; });
        std::vector<double> r(n);
        for (int i = 0; i < n; ++i) r[idx[i]] = i + 1.0;
        return r;
    };

    auto rs = rank_of(scores);
    auto rr = rank_of(fwd_rets);

    double d2 = 0;
    for (int i = 0; i < n; ++i) d2 += (rs[i]-rr[i])*(rs[i]-rr[i]);
    return 1.0 - 6*d2 / (n*(n*n-1.0));
}

// ── IC decay across holding periods ───────────────────────────────────────
struct ICDecay {
    std::vector<int>    horizons;   // forecast horizons (bars)
    std::vector<double> ic;        // IC at each horizon
    std::vector<double> icir;      // IC / IC_std
    int                 half_life = 0; // bars to half IC
};

// ═══════════════════════════════════════════════════════════════════════════
// ALPHA ENGINE
// ═══════════════════════════════════════════════════════════════════════════
struct AlphaEngineConfig {
    bool   parallel      = true;
    bool   winsorize     = true;
    double winsor_z      = 3.0;
    bool   rank_normalize= true;
    int    min_universe  = 20;
};

class AlphaEngine {
public:
    explicit AlphaEngine(AlphaEngineConfig cfg = AlphaEngineConfig()) : cfg_(cfg) {}

    void add_factor(std::shared_ptr<IFactor> f) { factors_.push_back(std::move(f)); }

    // Compute all factors for universe at given timestamp
    // Returns: factor_name → FactorResult
    std::unordered_map<std::string, FactorResult>
    compute(const std::vector<std::shared_ptr<DataFeed>>& feeds,
            Timestamp ts, int bar_window = 300)
    {
        std::unordered_map<std::string, FactorResult> results;

        for (auto& factor : factors_) {
            FactorResult fr;
            fr.reserve(feeds.size());

            if (cfg_.parallel) {
                std::vector<std::future<std::optional<FactorValue>>> futs;
                for (auto& feed : feeds) {
                    futs.push_back(global_pool().submit([&, feed]() {
                        return compute_one(*factor, *feed, ts, bar_window);
                    }));
                }
                for (auto& f : futs) {
                    auto v = f.get();
                    if (v) fr.push_back(*v);
                }
            } else {
                for (auto& feed : feeds) {
                    auto v = compute_one(*factor, *feed, ts, bar_window);
                    if (v) fr.push_back(*v);
                }
            }

            if ((int)fr.size() < cfg_.min_universe) {
                results[factor->name()] = fr;
                continue;
            }

            // Cross-sectional normalization
            if (cfg_.winsorize) winsorize(fr, cfg_.winsor_z);
            if (cfg_.rank_normalize) rank_normalize(fr);
            else zscore_normalize(fr);

            results[factor->name()] = std::move(fr);
        }
        return results;
    }

    // Compute IC time series for a factor
    struct ICTimeSeries {
        std::vector<Timestamp> dates;
        std::vector<double>    ic;
        double mean_ic = 0, ic_std = 0, icir = 0;
    };

    ICTimeSeries compute_ic(
        const std::string& factor_name,
        const std::vector<std::shared_ptr<DataFeed>>& feeds,
        Timestamp start, Timestamp end,
        int rebal_freq_bars = 21,
        int fwd_return_bars = 21)
    {
        ICTimeSeries result;
        // Identify dates to evaluate
        if (feeds.empty()) return result;
        const auto& ref_bars = feeds[0]->bars();

        // Find bar indices in range
        for (std::size_t i = 0; i < ref_bars.size(); i += rebal_freq_bars) {
            Timestamp ts = ref_bars[i].ts_open;
            if (ts < start || ts > end) continue;

            // Get factor scores at ts
            std::vector<double> scores, fwd_rets;
            for (auto& feed : feeds) {
                auto v = compute_one_by_name(factor_name, *feed, ts, 300);
                if (!v) continue;
                // Get forward return
                auto fwd_ts = ts + static_cast<Timestamp>(fwd_return_bars) * NS_PER_DAY;
                auto& bars  = feed->bars();
                auto it0 = std::lower_bound(bars.begin(), bars.end(), ts,
                    [](const Bar& b, Timestamp t){ return b.ts_open < t; });
                auto it1 = std::lower_bound(bars.begin(), bars.end(), fwd_ts,
                    [](const Bar& b, Timestamp t){ return b.ts_open < t; });
                if (it0 == bars.end() || it1 == bars.end()) continue;
                double fwd = (it0->close > 0) ? std::log(it1->close/it0->close) : 0;
                scores.push_back(v->raw);
                fwd_rets.push_back(fwd);
            }

            if ((int)scores.size() < cfg_.min_universe) continue;
            double ic = spearman_ic(scores, fwd_rets);
            result.dates.push_back(ts);
            result.ic.push_back(ic);
        }

        if (!result.ic.empty()) {
            result.mean_ic = 0;
            for (double ic : result.ic) result.mean_ic += ic;
            result.mean_ic /= result.ic.size();
            double var = 0;
            for (double ic : result.ic) var += (ic-result.mean_ic)*(ic-result.mean_ic);
            result.ic_std = std::sqrt(var/result.ic.size());
            result.icir = (result.ic_std > 0) ? result.mean_ic/result.ic_std : 0;
        }
        return result;
    }

    const std::vector<std::shared_ptr<IFactor>>& factors() const { return factors_; }

private:
    std::optional<FactorValue> compute_one(const IFactor& factor,
                                            const DataFeed& feed,
                                            Timestamp ts, int window) const {
        auto& bars = feed.bars();
        auto it = std::upper_bound(bars.begin(), bars.end(), ts,
            [](Timestamp t, const Bar& b){ return t < b.ts_open; });
        if (it == bars.begin()) return std::nullopt;
        --it;
        int idx = (int)(it - bars.begin());
        int start = std::max(0, idx - window + 1);
        std::span<const Bar> sp(bars.data() + start, idx - start + 1);
        if ((int)sp.size() < factor.min_bars()) return std::nullopt;

        FactorValue fv;
        fv.instrument = feed.instrument();
        fv.ts         = ts;
        fv.raw        = factor.compute(sp);
        fv.score      = fv.raw;
        fv.percentile = 0;
        return fv;
    }

    std::optional<FactorValue> compute_one_by_name(const std::string& fname,
                                                    const DataFeed& feed,
                                                    Timestamp ts, int window) const {
        for (auto& f : factors_) {
            if (f->name() == fname) return compute_one(*f, feed, ts, window);
        }
        return std::nullopt;
    }

    AlphaEngineConfig cfg_;
    std::vector<std::shared_ptr<IFactor>> factors_;
};

} // namespace factors
} // namespace ql
