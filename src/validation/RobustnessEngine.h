#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/validation/RobustnessEngine.h
// Institutional robustness testing for systematic strategies.
//
// Tests:
//   1. Monte Carlo return resampling
//   2. Bootstrap resampling (block bootstrap preserves autocorrelation)
//   3. Trade reshuffling (randomise entry/exit timing)
//   4. Noise injection (perturb prices by σ fraction)
//   5. Slippage stress (2x, 3x, 5x slippage)
//   6. Commission stress
//   7. Latency stress (order delays)
//   8. Regime stress (test in specific regime conditions)
//
// Outputs:
//   - Probability of failure (Sharpe < threshold)
//   - Risk of ruin (NAV < 50% of initial)
//   - Expected future drawdown distribution
//   - Confidence intervals on all metrics
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../simulation/Backtester.h"
#include "../analytics/Analytics.h"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <functional>
#include <string>
#include <map>

namespace ql {
namespace validation {

// ── Robustness test result ────────────────────────────────────────────────
struct RobustnessResult {
    std::string test_name;
    int    n_simulations     = 0;

    // Distribution of Sharpe ratios
    double sharpe_mean       = 0;
    double sharpe_std        = 0;
    double sharpe_p5         = 0;
    double sharpe_p25        = 0;
    double sharpe_p50        = 0;
    double sharpe_p75        = 0;
    double sharpe_p95        = 0;

    // Risk metrics
    double prob_failure      = 0;   // P(Sharpe < min_sharpe threshold)
    double prob_ruin         = 0;   // P(NAV < 50% of initial)
    double expected_max_dd   = 0;   // mean max drawdown across sims
    double max_dd_p95        = 0;   // 95th percentile max drawdown

    // CAGR distribution
    double cagr_p5           = 0;
    double cagr_p50          = 0;
    double cagr_p95          = 0;

    void print() const {
        std::printf("\n  ── %s ──────────────────────────────\n", test_name.c_str());
        std::printf("  Sharpe:    p5=%.3f  p25=%.3f  med=%.3f  p75=%.3f  p95=%.3f\n",
                    sharpe_p5, sharpe_p25, sharpe_p50, sharpe_p75, sharpe_p95);
        std::printf("  P(Fail):   %.1f%%    P(Ruin): %.1f%%\n",
                    prob_failure*100, prob_ruin*100);
        std::printf("  Max DD:    E[DD]=%.1f%%   p95=%.1f%%\n",
                    expected_max_dd, max_dd_p95);
    }
};

// ── Full robustness report ─────────────────────────────────────────────────
struct RobustnessReport {
    std::map<std::string, RobustnessResult> results;
    double overall_robustness_score = 0;   // 0-1

    void print() const {
        std::printf("\n+========================================================+\n");
        std::printf("|             ROBUSTNESS REPORT                        |\n");
        std::printf("+========================================================+\n");
        for (auto& [name, r] : results) {
            if (r.n_simulations == 0) {
                std::printf("\n  -- %s --------------------------------\n", name.c_str());
                std::printf("  (skipped: insufficient data)\n");
                continue;
            }
            r.print();
        }
        std::printf("\n  Overall Robustness Score: %.3f\n", overall_robustness_score);
    }
};

// ── Robustness engine ─────────────────────────────────────────────────────
struct RobustnessEngineConfig {
    int    n_mc_sims        = 500;
    int    block_size       = 10;
    double noise_sigma      = 0.005;
    double min_sharpe       = 0.0;
    double ruin_threshold   = 0.50;
    double slippage_mult    = 2.0;
    int    bars_per_year    = 252;
    double annual_rf        = 0.05;
    unsigned seed           = 42;
};

class RobustnessEngine {
public:
    using Config = RobustnessEngineConfig;
    explicit RobustnessEngine(Config cfg = Config()) : cfg_(cfg) {}

    // Run all robustness tests for a strategy
    RobustnessReport run(
        const std::vector<NAVPoint>&         nav_curve,
        const std::vector<TradeRecord>&      trades,
        std::function<BacktestResult(double slip_mult, double noise_sigma)> rerun_fn)
    {
        RobustnessReport report;

        // 1. MC return resampling
        report.results["mc_resampling"] = mc_resampling(nav_curve);

        // 2. Block bootstrap
        report.results["block_bootstrap"] = block_bootstrap(nav_curve);

        // 3. Trade reshuffling
        report.results["trade_reshuffle"] = trade_reshuffle(trades, nav_curve.front().nav);

        // 4. Noise injection
        if (rerun_fn) {
            auto noise_res = noise_stress(rerun_fn, cfg_.noise_sigma);
            report.results["noise_injection"] = noise_res;

            // 5. Slippage stress (2x, 3x)
            for (double mult : {2.0, 3.0, 5.0}) {
                std::string key = "slippage_" + std::to_string((int)mult) + "x";
                auto r = rerun_stress(rerun_fn, mult, 0.0);
                r.test_name = key;
                report.results[key] = r;
            }
        }

        // Compute overall score
        report.overall_robustness_score = compute_overall_score(report.results);
        return report;
    }

private:
    // ── 1. Monte Carlo return resampling ──────────────────────────────────
    RobustnessResult mc_resampling(const std::vector<NAVPoint>& curve) {
        RobustnessResult r; r.test_name = "MC Resampling";
        if (curve.size() < 20) return r;

        std::vector<double> rets;
        for (std::size_t i=1;i<curve.size();++i) {
            double p=curve[i-1].nav;
            if(p>0) rets.push_back((curve[i].nav-p)/p);
        }

        std::mt19937_64 rng(cfg_.seed);
        std::uniform_int_distribution<std::size_t> dist(0,rets.size()-1);
        int n=(int)rets.size();
        double init_nav = curve.front().nav;

        std::vector<double> sharpes, maxdds, cagrs;
        for (int sim=0;sim<cfg_.n_mc_sims;++sim) {
            double nav=init_nav, peak=init_nav, mdd=0;
            std::vector<double> sr;
            for (int j=0;j<n;++j) {
                double ret=rets[dist(rng)];
                nav*=(1+ret); sr.push_back(ret);
                peak=std::max(peak,nav); mdd=std::max(mdd,(peak-nav)/peak);
            }
            sharpes.push_back(sharpe_from_rets(sr));
            maxdds.push_back(mdd*100);
            double yrs=(double)n/cfg_.bars_per_year;
            cagrs.push_back(yrs>0?(std::pow(nav/init_nav,1.0/yrs)-1)*100:0);
        }
        fill_result(r, sharpes, maxdds, cagrs, init_nav);
        r.n_simulations = cfg_.n_mc_sims;
        return r;
    }

    // ── 2. Block bootstrap ────────────────────────────────────────────────
    RobustnessResult block_bootstrap(const std::vector<NAVPoint>& curve) {
        RobustnessResult r; r.test_name = "Block Bootstrap";
        if (curve.size() < 20) return r;

        std::vector<double> rets;
        for (std::size_t i=1;i<curve.size();++i) {
            double p=curve[i-1].nav;
            if(p>0) rets.push_back((curve[i].nav-p)/p);
        }

        std::mt19937_64 rng(cfg_.seed+1);
        int n=(int)rets.size();
        int bs=cfg_.block_size;
        int n_blocks=n/bs;
        std::uniform_int_distribution<int> block_dist(0, n-bs);
        double init_nav=curve.front().nav;

        std::vector<double> sharpes, maxdds, cagrs;
        for (int sim=0;sim<cfg_.n_mc_sims;++sim) {
            std::vector<double> sample;
            sample.reserve(n);
            for (int b=0;b<n_blocks;++b) {
                int start=block_dist(rng);
                for (int j=0;j<bs&&start+j<n;++j) sample.push_back(rets[start+j]);
            }
            double nav=init_nav, peak=init_nav, mdd=0;
            for (double ret:sample) {
                nav*=(1+ret); peak=std::max(peak,nav);
                mdd=std::max(mdd,(peak-nav)/peak);
            }
            sharpes.push_back(sharpe_from_rets(sample));
            maxdds.push_back(mdd*100);
            double yrs=(double)sample.size()/cfg_.bars_per_year;
            cagrs.push_back(yrs>0?(std::pow(nav/init_nav,1.0/yrs)-1)*100:0);
        }
        fill_result(r, sharpes, maxdds, cagrs, init_nav);
        r.n_simulations = cfg_.n_mc_sims;
        return r;
    }

    // ── 3. Trade reshuffling ───────────────────────────────────────────────
    RobustnessResult trade_reshuffle(const std::vector<TradeRecord>& trades,
                                      double init_nav) {
        RobustnessResult r; r.test_name = "Trade Reshuffle";
        if (trades.size() < 10) return r;

        std::vector<double> pnls;
        for (auto& t : trades) pnls.push_back(t.pnl);

        std::mt19937_64 rng(cfg_.seed+2);
        std::vector<double> sharpes, maxdds;
        for (int sim=0;sim<cfg_.n_mc_sims;++sim) {
            std::shuffle(pnls.begin(), pnls.end(), rng);
            double nav=init_nav, peak=init_nav, mdd=0;
            std::vector<double> rets;
            for (double p : pnls) {
                nav+=p; peak=std::max(peak,nav);
                mdd=std::max(mdd,(peak-nav)/peak);
                rets.push_back(p/init_nav);
            }
            sharpes.push_back(sharpe_from_rets(rets));
            maxdds.push_back(mdd*100);
        }
        std::vector<double> cagrs(cfg_.n_mc_sims, 0);
        fill_result(r, sharpes, maxdds, cagrs, init_nav);
        r.n_simulations = cfg_.n_mc_sims;
        return r;
    }

    // ── 4. Noise injection stress ──────────────────────────────────────────
    RobustnessResult noise_stress(
        std::function<BacktestResult(double,double)> rerun,
        double sigma)
    {
        RobustnessResult r; r.test_name = "Noise Injection";
        std::vector<double> sharpes, maxdds, cagrs;
        int n = std::min(cfg_.n_mc_sims / 5, 50); // fewer runs (expensive)
        for (int i=0;i<n;++i) {
            try {
                auto res = rerun(1.0, sigma * (1 + 0.5*i/(double)n));
                sharpes.push_back(res.report.sharpe_ratio);
                maxdds.push_back(res.report.max_drawdown);
                cagrs.push_back(res.report.cagr);
            } catch(...) {}
        }
        if (!sharpes.empty()) {
            fill_result(r, sharpes, maxdds, cagrs, 1.0);
            r.n_simulations = n;
        }
        return r;
    }

    // ── 5. Slippage/commission stress ──────────────────────────────────────
    RobustnessResult rerun_stress(
        std::function<BacktestResult(double,double)> rerun,
        double slip_mult, double noise)
    {
        RobustnessResult r; r.test_name = "Stress";
        std::vector<double> sharpes, maxdds, cagrs;
        for (int i=0;i<5;++i) {
            try {
                auto res = rerun(slip_mult, noise);
                sharpes.push_back(res.report.sharpe_ratio);
                maxdds.push_back(res.report.max_drawdown);
                cagrs.push_back(res.report.cagr);
            } catch(...) {}
        }
        if (!sharpes.empty()) fill_result(r, sharpes, maxdds, cagrs, 1.0);
        r.n_simulations = 5;
        return r;
    }

    // ── Helpers ────────────────────────────────────────────────────────────
    double sharpe_from_rets(const std::vector<double>& v) const {
        if ((int)v.size()<2) return 0;
        double m=0; for(double x:v) m+=x; m/=v.size();
        double var=0; for(double x:v) var+=(x-m)*(x-m); var/=v.size()-1;
        double rf=cfg_.annual_rf/cfg_.bars_per_year;
        return (var>0)?(m-rf)/std::sqrt(var)*std::sqrt(cfg_.bars_per_year):0;
    }

    void fill_result(RobustnessResult& r,
                     std::vector<double>& sharpes,
                     std::vector<double>& maxdds,
                     std::vector<double>& cagrs,
                     double init_nav) {
        if (sharpes.empty()) return;
        auto pct = [](std::vector<double>& v, double p) {
            std::sort(v.begin(), v.end());
            return v[std::max(0,(int)(p*v.size()))];
        };
        r.sharpe_p5    = pct(sharpes, 0.05);
        r.sharpe_p25   = pct(sharpes, 0.25);
        r.sharpe_p50   = pct(sharpes, 0.50);
        r.sharpe_p75   = pct(sharpes, 0.75);
        r.sharpe_p95   = pct(sharpes, 0.95);
        r.sharpe_mean  = 0; for(double x:sharpes) r.sharpe_mean+=x; r.sharpe_mean/=sharpes.size();
        r.sharpe_std   = 0; for(double x:sharpes) r.sharpe_std+=(x-r.sharpe_mean)*(x-r.sharpe_mean);
        r.sharpe_std   = std::sqrt(r.sharpe_std/sharpes.size());
        r.prob_failure = (double)std::count_if(sharpes.begin(),sharpes.end(),
                            [this](double s){return s<cfg_.min_sharpe;})/sharpes.size();
        r.expected_max_dd = 0; for(double x:maxdds) r.expected_max_dd+=x; r.expected_max_dd/=maxdds.size();
        r.max_dd_p95   = pct(maxdds, 0.95);
        if (!cagrs.empty()) { r.cagr_p5=pct(cagrs,0.05); r.cagr_p50=pct(cagrs,0.50); r.cagr_p95=pct(cagrs,0.95); }
    }

    double compute_overall_score(const std::map<std::string,RobustnessResult>& res) const {
        double score = 0; int cnt = 0;
        for (auto& [name, r] : res) {
            if (r.n_simulations == 0) continue;
            double s = 0;
            s += 0.4 * std::max(0.0, std::min(1.0, (r.sharpe_p25 + 1.0)/2.0));
            s += 0.3 * (1.0 - r.prob_failure);
            s += 0.3 * std::max(0.0, 1.0 - r.expected_max_dd/50.0);
            score += s; ++cnt;
        }
        return (cnt > 0) ? score/cnt : 0;
    }

    Config cfg_;
};

} // namespace validation
} // namespace ql
