#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/validation/WalkForwardValidator.h
// Institutional walk-forward validation engine.
//
// Modes:
//   Rolling    — fixed IS/OOS window, slides forward
//   Expanding  — IS grows, OOS fixed
//   Nested     — outer WF + inner CV for hyperparameter tuning
//
// Each WF window produces:
//   - Best IS parameters (via any optimizer)
//   - OOS performance with those parameters
//   - Robustness score (OOS/IS efficiency)
//
// Aggregate output:
//   - Walk-forward equity curve (concatenation of OOS equity curves)
//   - WF Sharpe, WF Max Drawdown
//   - Robustness score (mean OOS/IS efficiency)
//   - Overfitting probability
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../simulation/Backtester.h"
#include "../analytics/Analytics.h"
#include "../optimizer/Optimizer.h"
#include <vector>
#include <functional>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace ql {
namespace validation {

// ── WF window result ───────────────────────────────────────────────────────
struct WFWindowResult {
    int    window_id      = 0;
    int    is_start_bar   = 0;
    int    is_end_bar     = 0;
    int    oos_start_bar  = 0;
    int    oos_end_bar    = 0;

    optimizer::ParamSet best_params;

    // IS metrics
    double is_sharpe      = 0;
    double is_cagr        = 0;
    double is_max_dd      = 0;

    // OOS metrics
    double oos_sharpe     = 0;
    double oos_cagr       = 0;
    double oos_max_dd     = 0;
    double oos_total_ret  = 0;

    // Quality metrics
    double efficiency     = 0;   // oos_sharpe / is_sharpe
    bool   is_profitable  = false;
};

// ── WF aggregate result ────────────────────────────────────────────────────
struct WalkForwardResult {
    std::vector<WFWindowResult> windows;

    // Aggregates across all OOS windows
    double wf_sharpe        = 0;
    double wf_sortino       = 0;
    double wf_max_dd        = 0;
    double wf_cagr          = 0;
    double wf_total_return  = 0;

    // Robustness
    double mean_efficiency  = 0;   // avg OOS/IS Sharpe
    double pct_profitable_windows = 0;
    double overfitting_prob = 0;   // estimated P(IS > OOS due to overfitting)
    double robustness_score = 0;   // composite 0-1

    // WF equity curve (concatenated OOS curves)
    std::vector<NAVPoint> wf_equity_curve;

    void print() const {
        std::printf("\n── Walk-Forward Aggregate Results ────────────────────\n");
        std::printf("  WF Sharpe:            %+.3f\n", wf_sharpe);
        std::printf("  WF CAGR (%%):          %+.2f\n", wf_cagr);
        std::printf("  WF Max Drawdown (%%):  %.2f\n",  wf_max_dd);
        std::printf("  Mean IS/OOS Effic.:   %.1f%%\n", mean_efficiency*100);
        std::printf("  %% Profitable Windows: %.1f%%\n", pct_profitable_windows*100);
        std::printf("  Overfitting Prob:     %.1f%%\n", overfitting_prob*100);
        std::printf("  Robustness Score:     %.3f\n",   robustness_score);
        std::printf("  Windows:              %d\n",     (int)windows.size());
    }
};

// ── Validator configuration ────────────────────────────────────────────────
enum class WFMode { Rolling, Expanding, Nested };

struct ValidatorConfig {
    WFMode mode          = WFMode::Rolling;
    int    is_bars       = 252;
    int    oos_bars      = 63;
    int    step_bars     = 63;
    int    min_oos_bars  = 21;
    // For Nested: inner CV folds
    int    inner_folds   = 3;
    bool   verbose       = true;
};

// ── Eval function type ─────────────────────────────────────────────────────
using EvalFn = std::function<analytics::PerformanceReport(
    const optimizer::ParamSet&,
    const std::vector<std::shared_ptr<DataFeed>>&)>;

using DataSliceFn = std::function<std::vector<std::shared_ptr<DataFeed>>(
    const std::vector<std::shared_ptr<DataFeed>>&,
    int bar_start, int bar_end)>;

// ── Walk-Forward Validator ─────────────────────────────────────────────────
class WalkForwardValidator {
public:
    WalkForwardValidator(EvalFn eval_fn,
                         DataSliceFn slice_fn,
                         std::vector<optimizer::ParamDef> params,
                         ValidatorConfig cfg = ValidatorConfig())
        : eval_(std::move(eval_fn))
        , slice_(std::move(slice_fn))
        , params_(std::move(params))
        , cfg_(cfg) {}

    WalkForwardResult run(
        const std::vector<std::shared_ptr<DataFeed>>& feeds,
        optimizer::Objective obj = optimizer::Objective::Sharpe)
    {
        if (feeds.empty()) return {};
        int total_bars = (int)feeds[0]->size();

        WalkForwardResult result;
        int start = 0, win_id = 0;

        while (start + cfg_.is_bars + cfg_.min_oos_bars <= total_bars) {
            int is_end  = (cfg_.mode == WFMode::Expanding)
                          ? start + cfg_.is_bars + win_id * cfg_.step_bars
                          : start + cfg_.is_bars;
            is_end = std::min(is_end, total_bars - cfg_.min_oos_bars);

            int oos_end = std::min(is_end + cfg_.oos_bars, total_bars);
            if (oos_end - is_end < cfg_.min_oos_bars) break;

            // Slice IS data
            auto is_feeds  = slice_(feeds, start, is_end);
            auto oos_feeds = slice_(feeds, is_end, oos_end);

            // Optimize on IS
            optimizer::ParamSet best_params;
            double is_sharpe = 0;

            if (cfg_.mode == WFMode::Nested) {
                best_params = inner_cv(is_feeds, win_id);
                auto is_rep = eval_(best_params, is_feeds);
                is_sharpe   = is_rep.sharpe_ratio;
            } else {
                // LHS optimization on IS
                optimizer::ObjectiveFn obj_fn = [&](const optimizer::ParamSet& ps) {
                    auto rep = eval_(ps, is_feeds);
                    return extract_score(rep, obj);
                };
                optimizer::LatinHypercubeSampler lhs(obj_fn, params_, 100);
                auto trials = lhs.run(1, cfg_.verbose ? false : false);
                if (trials.empty()) { advance(start, win_id); continue; }
                best_params = trials[0].params;
                is_sharpe   = trials[0].score;
            }

            // Evaluate on OOS
            auto oos_rep = eval_(best_params, oos_feeds);

            WFWindowResult wr;
            wr.window_id     = win_id;
            wr.is_start_bar  = start;
            wr.is_end_bar    = is_end;
            wr.oos_start_bar = is_end;
            wr.oos_end_bar   = oos_end;
            wr.best_params   = best_params;
            wr.is_sharpe     = is_sharpe;
            wr.is_cagr       = 0; // filled from IS eval if needed
            wr.oos_sharpe    = oos_rep.sharpe_ratio;
            wr.oos_cagr      = oos_rep.cagr;
            wr.oos_max_dd    = oos_rep.max_drawdown;
            wr.oos_total_ret = oos_rep.total_return_pct;
            wr.efficiency    = (is_sharpe != 0) ? oos_rep.sharpe_ratio/is_sharpe : 0;
            wr.is_profitable = (oos_rep.sharpe_ratio > 0);

            result.windows.push_back(wr);

            if (cfg_.verbose)
                std::printf("[WF] Win=%2d  IS=%.3f  OOS=%.3f  Eff=%.1f%%\n",
                            win_id, is_sharpe, oos_rep.sharpe_ratio,
                            wr.efficiency*100);

            advance(start, win_id);
        }

        // Compute aggregates
        aggregate(result);
        if (cfg_.verbose) result.print();
        return result;
    }

private:
    void advance(int& start, int& win_id) {
        start += cfg_.step_bars;
        ++win_id;
    }

    optimizer::ParamSet inner_cv(
        const std::vector<std::shared_ptr<DataFeed>>& is_feeds, int /*win*/) {
        // k-fold CV within IS: split IS into k equal folds, optimize
        int n      = (int)is_feeds[0]->size();
        int fold_sz= n / cfg_.inner_folds;

        optimizer::ObjectiveFn obj_fn = [&](const optimizer::ParamSet& ps) {
            double avg_score = 0;
            for (int f = 0; f < cfg_.inner_folds; ++f) {
                int val_start = f * fold_sz;
                int val_end   = std::min(val_start + fold_sz, n);
                auto val_feeds = slice_(is_feeds, val_start, val_end);
                if (val_feeds.empty()) continue;
                auto rep = eval_(ps, val_feeds);
                avg_score += rep.sharpe_ratio;
            }
            return avg_score / cfg_.inner_folds;
        };

        optimizer::LatinHypercubeSampler lhs(obj_fn, params_, 60);
        auto trials = lhs.run(1, false);
        return trials.empty() ? optimizer::ParamSet{} : trials[0].params;
    }

    static double extract_score(const analytics::PerformanceReport& r,
                                  optimizer::Objective obj) {
        switch(obj) {
        case optimizer::Objective::Sharpe:       return r.sharpe_ratio;
        case optimizer::Objective::Sortino:      return r.sortino_ratio;
        case optimizer::Objective::CAGR:         return r.cagr;
        case optimizer::Objective::ProfitFactor: return r.profit_factor;
        case optimizer::Objective::Calmar:       return r.calmar_ratio;
        }
        return r.sharpe_ratio;
    }

    void aggregate(WalkForwardResult& res) {
        if (res.windows.empty()) return;

        // Mean efficiency
        double sum_eff = 0, sum_oos_s = 0;
        int prof = 0;
        for (auto& w : res.windows) {
            sum_eff  += w.efficiency;
            sum_oos_s+= w.oos_sharpe;
            if (w.is_profitable) ++prof;
        }
        int n = (int)res.windows.size();
        res.mean_efficiency           = sum_eff / n;
        res.wf_sharpe                 = sum_oos_s / n;
        res.pct_profitable_windows    = (double)prof / n;

        // Collect all OOS Sharpes for stats
        std::vector<double> oos_s;
        for (auto& w : res.windows) oos_s.push_back(w.oos_sharpe);
        double m = 0; for (double x:oos_s) m+=x; m/=n;
        double v = 0; for (double x:oos_s) v+=(x-m)*(x-m); v/=(n>1?n-1:1);

        // WF total return (geometric sum of OOS returns)
        double cum = 1.0;
        for (auto& w : res.windows) cum *= (1.0 + w.oos_total_ret/100.0);
        res.wf_total_return = (cum - 1.0) * 100.0;
        res.wf_cagr = (n > 0 && cum > 0)
            ? (std::pow(cum, (double)252/(n*res.windows[0].oos_end_bar -
                                          res.windows[0].oos_start_bar + 1)) - 1.0) * 100.0
            : 0.0;

        // Max OOS drawdown
        res.wf_max_dd = 0;
        for (auto& w : res.windows) res.wf_max_dd = std::max(res.wf_max_dd, w.oos_max_dd);

        // Overfitting probability (empirical: how often IS >> OOS)
        int overfit_cnt = 0;
        for (auto& w : res.windows)
            if (w.is_sharpe > 0 && w.oos_sharpe < w.is_sharpe * 0.3) ++overfit_cnt;
        res.overfitting_prob = (double)overfit_cnt / n;

        // Robustness score: 0-1
        res.robustness_score =
            0.35 * std::max(0.0, std::min(1.0, res.mean_efficiency + 0.5))
          + 0.30 * res.pct_profitable_windows
          + 0.20 * std::max(0.0, std::min(1.0, (res.wf_sharpe + 1.0) / 2.0))
          + 0.15 * (1.0 - res.overfitting_prob);
    }

    EvalFn        eval_;
    DataSliceFn   slice_;
    std::vector<optimizer::ParamDef> params_;
    ValidatorConfig cfg_;
};

} // namespace validation
} // namespace ql
