#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/ranking/StrategyRanker.h
// Institutional strategy ranking system.
//
// Every strategy entering the ranker carries:
//   - Performance metrics (from analytics::PerformanceReport)
//   - Walk-forward results (from validation::WalkForwardResult)
//   - Robustness results (from validation::RobustnessReport)
//   - Regime performance (per-regime Sharpe array)
//   - Return series (for correlation-to-portfolio computation)
//
// Outputs two scores per strategy:
//   - Composite Score: weighted blend of risk-adjusted performance metrics
//   - Institutional Fitness Score: composite + robustness + regime
//     consistency + diversification benefit, suitable for capital allocation
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../analytics/Analytics.h"
#include "../validation/WalkForwardValidator.h"
#include "../validation/RobustnessEngine.h"
#include "../regimes/RegimeEngine.h"
#include <vector>
#include <string>
#include <array>
#include <optional>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstdio>

namespace ql {
namespace ranking {

// ── Strategy entry into the ranker ─────────────────────────────────────────
struct StrategyEntry {
    std::string id;
    std::string name;

    analytics::PerformanceReport perf;

    std::optional<validation::WalkForwardResult>  wf;
    std::optional<validation::RobustnessReport>   robustness;

    std::array<double,9> regime_sharpe = {};
    std::array<int,9>    regime_bars   = {};

    std::vector<double> returns;
    double capacity_score = 0.5;

    // ── Computed scores (filled by rank()) ─────────────────────────────────
    double composite_score          = 0;
    double regime_stability_cache   = 0;
    double fitness_score            = 0;
    double correlation_to_book      = 0;
    double diversification_benefit  = 0;
    int    composite_rank           = 0;
    int    fitness_rank             = 0;
};

// ── Scoring weights ─────────────────────────────────────────────────────────
struct RankerWeights {
    double w_sharpe        = 0.25;
    double w_sortino       = 0.10;
    double w_calmar        = 0.15;
    double w_max_dd        = 0.15;
    double w_consistency   = 0.10;
    double w_oos_sharpe    = 0.15;
    double w_robustness    = 0.10;

    double f_composite        = 0.40;
    double f_regime_stability = 0.20;
    double f_diversification  = 0.20;
    double f_capacity         = 0.10;
    double f_turnover_penalty = 0.10;
};

// ── Strategy ranker ──────────────────────────────────────────────────────────
class StrategyRanker {
public:
    explicit StrategyRanker(RankerWeights w = RankerWeights()) : w_(w) {}

    void add(StrategyEntry entry) { entries_.push_back(std::move(entry)); }
    std::size_t size() const { return entries_.size(); }

    void rank(const std::vector<double>* portfolio_returns = nullptr) {
        for (auto& e : entries_) {
            e.composite_score        = composite(e);
            e.regime_stability_cache = regime_stability(e);
            if (portfolio_returns)
                e.correlation_to_book = correlation(e.returns, *portfolio_returns);
            e.diversification_benefit = 1.0 - std::abs(e.correlation_to_book);
            e.fitness_score = fitness(e);
        }

        std::vector<StrategyEntry*> by_comp;
        for (auto& e : entries_) by_comp.push_back(&e);
        std::sort(by_comp.begin(), by_comp.end(),
                  [](auto* a, auto* b){ return a->composite_score > b->composite_score; });
        for (int i=0;i<(int)by_comp.size();++i) by_comp[i]->composite_rank = i+1;

        std::vector<StrategyEntry*> by_fit;
        for (auto& e : entries_) by_fit.push_back(&e);
        std::sort(by_fit.begin(), by_fit.end(),
                  [](auto* a, auto* b){ return a->fitness_score > b->fitness_score; });
        for (int i=0;i<(int)by_fit.size();++i) by_fit[i]->fitness_rank = i+1;
    }

    std::vector<const StrategyEntry*> qualified(double min_fitness = 0.4) const {
        std::vector<const StrategyEntry*> out;
        for (auto& e : entries_)
            if (e.fitness_score >= min_fitness) out.push_back(&e);
        std::sort(out.begin(), out.end(),
                  [](auto* a, auto* b){ return a->fitness_score > b->fitness_score; });
        return out;
    }

    const std::vector<StrategyEntry>& all() const { return entries_; }

    void print(int top_n = 20) const {
        std::vector<const StrategyEntry*> sorted;
        for (auto& e : entries_) sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b){ return a->fitness_score > b->fitness_score; });

        std::printf("\n+======================================================================+\n");
        std::printf("|                       STRATEGY RANKING                              |\n");
        std::printf("+======================================================================+\n");
        std::printf("  %-22s  %7s  %7s  %7s  %7s  %7s  %5s\n",
                    "Strategy","Sharpe","MaxDD%","OOS-Sh","Compos","Fitness","Rank");
        std::printf("  %s\n", std::string(72,'-').c_str());

        for (int i=0;i<std::min(top_n,(int)sorted.size());++i) {
            auto* e = sorted[i];
            double oos = e->wf ? e->wf->wf_sharpe : 0.0;
            std::printf("  %-22s  %7.3f  %7.2f  %7.3f  %7.4f  %7.4f  %5d\n",
                e->name.c_str(), e->perf.sharpe_ratio, e->perf.max_drawdown,
                oos, e->composite_score, e->fitness_score, e->fitness_rank);
        }
    }

private:
    double composite(const StrategyEntry& e) const {
        auto norm_sharpe = [](double s) { return std::clamp((s+1.0)/4.0, 0.0, 1.0); };
        auto norm_dd     = [](double dd){ return std::clamp(1.0 - dd/40.0, 0.0, 1.0); };
        auto norm_calmar = [](double c) { return std::clamp(c/3.0, 0.0, 1.0); };

        double consistency = std::clamp(
            0.5*e.perf.win_rate/100.0 + 0.5*std::min(e.perf.profit_factor/2.5,1.0),
            0.0, 1.0);

        double oos_sharpe = e.wf ? e.wf->wf_sharpe : e.perf.sharpe_ratio * 0.5;
        double robustness = e.robustness ? e.robustness->overall_robustness_score : 0.5;

        double s = 0;
        s += w_.w_sharpe      * norm_sharpe(e.perf.sharpe_ratio);
        s += w_.w_sortino     * norm_sharpe(e.perf.sortino_ratio);
        s += w_.w_calmar      * norm_calmar(e.perf.calmar_ratio);
        s += w_.w_max_dd      * norm_dd(e.perf.max_drawdown);
        s += w_.w_consistency * consistency;
        s += w_.w_oos_sharpe  * norm_sharpe(oos_sharpe);
        s += w_.w_robustness  * robustness;
        return s;
    }

    double regime_stability(const StrategyEntry& e) const {
        int n_active = 0, n_positive = 0;
        double sum_sharpe = 0;
        for (int i=0;i<9;++i) {
            if (e.regime_bars[i] < 10) continue;
            ++n_active;
            sum_sharpe += e.regime_sharpe[i];
            if (e.regime_sharpe[i] > 0) ++n_positive;
        }
        if (n_active == 0) return 0.5;
        double pct_positive = (double)n_positive / n_active;
        double avg_sharpe   = sum_sharpe / n_active;
        double var = 0;
        for (int i=0;i<9;++i) {
            if (e.regime_bars[i] < 10) continue;
            var += (e.regime_sharpe[i]-avg_sharpe)*(e.regime_sharpe[i]-avg_sharpe);
        }
        var /= n_active;
        double consistency_penalty = std::clamp(1.0 - std::sqrt(var)/3.0, 0.0, 1.0);
        return 0.5*pct_positive + 0.5*consistency_penalty;
    }

    double fitness(const StrategyEntry& e) const {
        double turnover_penalty = std::clamp(
            1.0 - e.perf.turnover_annual/2000.0, 0.0, 1.0);

        double f = 0;
        f += w_.f_composite        * e.composite_score;
        f += w_.f_regime_stability * e.regime_stability_cache;
        f += w_.f_diversification  * e.diversification_benefit;
        f += w_.f_capacity         * e.capacity_score;
        f += w_.f_turnover_penalty * turnover_penalty;
        return f;
    }

    static double correlation(const std::vector<double>& a,
                               const std::vector<double>& b) {
        if (a.empty() || b.empty()) return 0;
        int n = (int)std::min(a.size(), b.size());
        if (n < 5) return 0;
        double ma=0, mb=0;
        for (int i=0;i<n;++i) { ma+=a[i]; mb+=b[i]; }
        ma/=n; mb/=n;
        double num=0, va=0, vb=0;
        for (int i=0;i<n;++i) {
            num += (a[i]-ma)*(b[i]-mb);
            va  += (a[i]-ma)*(a[i]-ma);
            vb  += (b[i]-mb)*(b[i]-mb);
        }
        return (va*vb>0) ? num/std::sqrt(va*vb) : 0;
    }

    RankerWeights w_;
    std::vector<StrategyEntry> entries_;
};

} // namespace ranking
} // namespace ql
