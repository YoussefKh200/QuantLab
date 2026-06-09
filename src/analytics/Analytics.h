#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/analytics/Analytics.h
// Comprehensive performance analytics.
//
// Metrics: Sharpe, Sortino, Calmar, MAR, Ulcer Index, Omega Ratio,
//          Information Ratio, Treynor, Alpha, Beta, Profit Factor,
//          Expectancy, CAGR, Turnover, Hit Rate, Factor Attribution
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../portfolio/Portfolio.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <numeric>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>
#include <optional>
#include <random>

namespace ql {
namespace analytics {

// ── Full performance report ────────────────────────────────────────────────
struct PerformanceReport {
    // ── Return metrics ─────────────────────────────────────────────────────
    double total_return_pct      = 0;
    double cagr                  = 0;
    double annualized_vol        = 0;

    // ── Risk-adjusted ──────────────────────────────────────────────────────
    double sharpe_ratio          = 0;
    double sortino_ratio         = 0;
    double calmar_ratio          = 0;
    double mar_ratio             = 0;   // CAGR / max_drawdown
    double omega_ratio           = 0;   // P(gain>threshold) / P(loss>threshold)
    double ulcer_index           = 0;   // RMS of drawdowns

    // ── Drawdown ───────────────────────────────────────────────────────────
    double max_drawdown          = 0;
    double avg_drawdown          = 0;
    double max_drawdown_duration = 0;   // bars
    double avg_drawdown_duration = 0;
    double recovery_factor       = 0;   // total_return / max_drawdown

    // ── Trade statistics ───────────────────────────────────────────────────
    int    total_trades          = 0;
    int    winning_trades        = 0;
    int    losing_trades         = 0;
    double win_rate              = 0;
    double avg_win               = 0;
    double avg_loss              = 0;
    double best_trade            = 0;
    double worst_trade           = 0;
    double profit_factor         = 0;
    double expectancy            = 0;   // expected $ per trade
    double avg_holding_bars      = 0;

    // ── Market exposure ────────────────────────────────────────────────────
    double avg_gross_leverage    = 0;
    double avg_net_exposure      = 0;
    double turnover_annual       = 0;   // % of portfolio turned per year

    // ── Benchmark-relative ────────────────────────────────────────────────
    double alpha                 = 0;
    double beta                  = 0;
    double correlation           = 0;
    double tracking_error        = 0;
    double information_ratio     = 0;
    double treynor_ratio         = 0;
    double up_capture            = 0;
    double down_capture          = 0;

    // ── Costs ─────────────────────────────────────────────────────────────
    double total_commission      = 0;
    double total_slippage        = 0;
    double costs_pct_return      = 0;   // costs as % of gross return

    // ── Summary ───────────────────────────────────────────────────────────
    double initial_nav           = 0;
    double final_nav             = 0;
    int    total_bars            = 0;
    double bars_per_year         = 252;
};

// ── Monte Carlo results ───────────────────────────────────────────────────
struct MonteCarloResult {
    double median_final_nav;
    double p5_final_nav;
    double p25_final_nav;
    double p75_final_nav;
    double p95_final_nav;
    double p95_max_drawdown;
    double p5_sharpe;
    double median_sharpe;
    double prob_profitable;       // % sims ending above initial
    double expected_shortfall_5;  // average of worst 5%
};

// ── Analytics engine ──────────────────────────────────────────────────────
class AnalyticsEngine {
public:
    // ── Main compute ──────────────────────────────────────────────────────
    static PerformanceReport compute(
        const Portfolio& pf,
        double annual_rf        = 0.05,
        int    bars_per_year    = 252,
        const std::vector<double>* benchmark_rets = nullptr)
    {
        PerformanceReport r;
        const auto& curve  = pf.nav_curve();
        const auto& trades = pf.trades();

        if (curve.size() < 2) return r;

        r.initial_nav   = pf.initial_cash();
        r.final_nav     = curve.back().nav;
        r.total_bars    = (int)curve.size();
        r.bars_per_year = bars_per_year;
        r.total_commission = pf.commission_total();

        // ── Return series ──────────────────────────────────────────────────
        auto rets = extract_returns(curve);
        if (rets.empty()) return r;

        double rf_d     = annual_rf / bars_per_year;
        double mean_r   = mean(rets);
        double vol_d    = std_dev(rets);
        double years    = (double)rets.size() / bars_per_year;

        // ── Absolute return metrics ────────────────────────────────────────
        r.total_return_pct = (r.final_nav / r.initial_nav - 1.0) * 100.0;
        r.cagr = (years > 0)
                 ? (std::pow(r.final_nav / r.initial_nav, 1.0/years) - 1.0) * 100.0
                 : 0.0;
        r.annualized_vol = vol_d * std::sqrt(bars_per_year) * 100.0;

        // ── Risk-adjusted ──────────────────────────────────────────────────
        double excess = mean_r - rf_d;
        r.sharpe_ratio  = (vol_d > 0)
                          ? excess / vol_d * std::sqrt(bars_per_year) : 0;
        r.sortino_ratio = sortino(rets, rf_d, bars_per_year);
        r.omega_ratio   = omega(rets, rf_d);
        r.ulcer_index   = ulcer(curve);

        // ── Drawdown ──────────────────────────────────────────────────────
        auto [mdd, mdd_dur, avg_dd, avg_dd_dur] = drawdown_stats(curve);
        r.max_drawdown           = mdd * 100.0;
        r.avg_drawdown           = avg_dd * 100.0;
        r.max_drawdown_duration  = mdd_dur;
        r.avg_drawdown_duration  = avg_dd_dur;
        r.calmar_ratio  = (mdd > 0) ? (r.cagr/100.0) / mdd : 0;
        r.mar_ratio     = r.calmar_ratio;
        r.recovery_factor = (mdd > 0) ? (r.total_return_pct/100.0) / mdd : 0;

        // ── Trade stats ───────────────────────────────────────────────────
        r.total_trades = (int)trades.size();
        double gross_profit = 0, gross_loss = 0, total_hold = 0;
        for (auto& t : trades) {
            if (t.pnl >= 0) {
                ++r.winning_trades;
                r.avg_win += t.pnl;
                gross_profit += t.pnl;
                r.best_trade = std::max(r.best_trade, t.pnl);
            } else {
                ++r.losing_trades;
                r.avg_loss += t.pnl;
                gross_loss += std::abs(t.pnl);
                r.worst_trade = std::min(r.worst_trade, t.pnl);
            }
            total_hold += t.holding_period_bars;
        }
        if (r.total_trades > 0) {
            r.win_rate = (double)r.winning_trades / r.total_trades * 100.0;
            r.avg_holding_bars = total_hold / r.total_trades;
        }
        if (r.winning_trades > 0) r.avg_win /= r.winning_trades;
        if (r.losing_trades  > 0) r.avg_loss /= r.losing_trades;
        r.profit_factor = (gross_loss > 0) ? gross_profit/gross_loss
                        : (gross_profit > 0 ? 9999 : 0);
        r.expectancy = (r.win_rate/100.0) * r.avg_win
                     + (1-r.win_rate/100.0) * r.avg_loss;

        // ── Exposure ──────────────────────────────────────────────────────
        double sum_lev = 0, sum_net = 0;
        for (auto& p : curve) { sum_lev += p.leverage; sum_net += p.net_exposure; }
        r.avg_gross_leverage = sum_lev / curve.size();
        r.avg_net_exposure   = sum_net / curve.size();

        // ── Benchmark-relative ────────────────────────────────────────────
        if (benchmark_rets && benchmark_rets->size() >= rets.size()) {
            std::vector<double> bret(benchmark_rets->begin(),
                                     benchmark_rets->begin() + rets.size());
            r.beta        = beta_calc(rets, bret);
            r.alpha       = (r.cagr/100.0 - annual_rf)
                          - r.beta * (mean(bret)*bars_per_year - annual_rf);
            r.alpha      *= 100.0;
            r.correlation = pearson_corr(rets, bret);
            std::vector<double> active;
            for (std::size_t i = 0; i < rets.size(); ++i)
                active.push_back(rets[i] - bret[i]);
            double te = std_dev(active) * std::sqrt(bars_per_year);
            r.tracking_error = te * 100.0;
            double active_mean = mean(active) * bars_per_year;
            r.information_ratio = (te > 0) ? active_mean/te : 0;
            r.treynor_ratio = (r.beta != 0)
                ? (r.cagr/100.0 - annual_rf) / std::abs(r.beta) : 0;
            up_down_capture(rets, bret, r.up_capture, r.down_capture);
        }

        return r;
    }

    // ── Monte Carlo ───────────────────────────────────────────────────────
    static MonteCarloResult monte_carlo(
        const Portfolio& pf,
        int simulations  = 2000,
        int bars_per_year= 252,
        double annual_rf = 0.05,
        unsigned seed    = 42)
    {
        auto rets = extract_returns(pf.nav_curve());
        if (rets.size() < 20) return {};

        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<std::size_t> dist(0, rets.size()-1);

        int n = (int)rets.size();
        double init_nav = pf.initial_cash();

        std::vector<double> finals, mdds, sharpes;
        finals.reserve(simulations);
        mdds.reserve(simulations);

        for (int sim = 0; sim < simulations; ++sim) {
            double nav = init_nav, peak = init_nav, mdd = 0;
            std::vector<double> sim_rets;
            sim_rets.reserve(n);
            for (int j = 0; j < n; ++j) {
                double r = rets[dist(rng)];
                nav *= (1.0 + r);
                peak = std::max(peak, nav);
                mdd  = std::max(mdd, (peak-nav)/peak);
                sim_rets.push_back(r);
            }
            finals.push_back(nav);
            mdds.push_back(mdd);
            double rf_d = annual_rf / bars_per_year;
            double m = mean(sim_rets), s = std_dev(sim_rets);
            double sh = (s > 0) ? (m-rf_d)/s*std::sqrt(bars_per_year) : 0;
            sharpes.push_back(sh);
        }

        auto pct = [](std::vector<double>& v, double p) {
            std::sort(v.begin(), v.end());
            return v[std::max(0, (int)(p*v.size()))];
        };

        MonteCarloResult mc;
        mc.p5_final_nav      = pct(finals, 0.05);
        mc.p25_final_nav     = pct(finals, 0.25);
        mc.median_final_nav  = pct(finals, 0.50);
        mc.p75_final_nav     = pct(finals, 0.75);
        mc.p95_final_nav     = pct(finals, 0.95);
        mc.p95_max_drawdown  = pct(mdds,   0.95) * 100.0;
        mc.p5_sharpe         = pct(sharpes, 0.05);
        mc.median_sharpe     = pct(sharpes, 0.50);
        mc.prob_profitable   = 100.0 * std::count_if(finals.begin(), finals.end(),
            [init_nav](double v){ return v > init_nav; }) / simulations;

        // Expected shortfall of worst 5%
        std::sort(finals.begin(), finals.end());
        int cutoff = simulations / 20;
        double es = 0;
        for (int i = 0; i < cutoff; ++i) es += finals[i];
        mc.expected_shortfall_5 = (cutoff > 0) ? es/cutoff : finals[0];

        return mc;
    }

    // ── Pretty-print tearsheet ────────────────────────────────────────────
    static std::string format(const PerformanceReport& r,
                               const std::string& title = "PERFORMANCE REPORT") {
        std::ostringstream ss;
        auto w = [&](const char* lbl, double v, const char* fmt = "%+.2f") {
            char buf[64]; std::snprintf(buf, sizeof(buf), fmt, v);
            ss << "  " << std::left << std::setw(30) << lbl
               << std::right << std::setw(12) << buf << "\n";
        };
        auto wi = [&](const char* lbl, int v) {
            ss << "  " << std::left << std::setw(30) << lbl
               << std::right << std::setw(12) << v << "\n";
        };
        auto sep = [&](const char* s = "") {
            ss << "  ── " << s << " ";
            int pad = 42 - 4 - (int)strlen(s) - 1;
            while (pad-- > 0) ss << '-';
            ss << "\n";
        };

        ss << "\n╔══════════════════════════════════════════════╗\n";
        ss << "║  " << std::left << std::setw(44) << title << "║\n";
        ss << "╚══════════════════════════════════════════════╝\n";

        sep("Returns");
        w("Initial NAV ($)",        r.initial_nav,      "%.2f");
        w("Final NAV ($)",          r.final_nav,        "%.2f");
        w("Total Return (%)",       r.total_return_pct);
        w("CAGR (%)",               r.cagr);
        w("Annualized Vol (%)",     r.annualized_vol,   "%.2f");

        sep("Risk-Adjusted");
        w("Sharpe Ratio",           r.sharpe_ratio,     "%.3f");
        w("Sortino Ratio",          r.sortino_ratio,    "%.3f");
        w("Calmar Ratio",           r.calmar_ratio,     "%.3f");
        w("Omega Ratio",            r.omega_ratio,      "%.3f");
        w("Ulcer Index",            r.ulcer_index,      "%.3f");

        sep("Drawdown");
        w("Max Drawdown (%)",       r.max_drawdown,     "%.2f");
        w("Avg Drawdown (%)",       r.avg_drawdown,     "%.2f");
        w("Max DD Duration (bars)", r.max_drawdown_duration, "%.0f");
        w("Recovery Factor",        r.recovery_factor,  "%.2f");

        sep("Trades");
        wi("Total Trades",          r.total_trades);
        wi("Winning",               r.winning_trades);
        wi("Losing",                r.losing_trades);
        w("Win Rate (%)",           r.win_rate,         "%.1f");
        w("Avg Win ($)",            r.avg_win,          "%.2f");
        w("Avg Loss ($)",           r.avg_loss,         "%.2f");
        w("Best Trade ($)",         r.best_trade,       "%.2f");
        w("Worst Trade ($)",        r.worst_trade,      "%.2f");
        w("Profit Factor",          r.profit_factor,    "%.3f");
        w("Expectancy ($/trade)",   r.expectancy,       "%.2f");
        w("Avg Hold (bars)",        r.avg_holding_bars, "%.1f");

        sep("Exposure");
        w("Avg Gross Leverage",     r.avg_gross_leverage,"%.2f");
        w("Avg Net Exposure",       r.avg_net_exposure, "%.2f");

        if (r.alpha != 0 || r.beta != 0) {
            sep("Benchmark-Relative");
            w("Alpha (ann. %)",         r.alpha);
            w("Beta",                   r.beta,         "%.3f");
            w("Correlation",            r.correlation,  "%.3f");
            w("Tracking Error (%)",     r.tracking_error,"%.2f");
            w("Information Ratio",      r.information_ratio,"%.3f");
            w("Treynor Ratio",          r.treynor_ratio,"%.3f");
            w("Up Capture (%)",         r.up_capture,   "%.1f");
            w("Down Capture (%)",       r.down_capture, "%.1f");
        }

        sep("Costs");
        w("Total Commission ($)",   r.total_commission, "%.2f");
        ss << "══════════════════════════════════════════════════\n";
        return ss.str();
    }

    static std::string format_mc(const MonteCarloResult& mc, int sims) {
        std::ostringstream ss;
        ss << "\n── Monte Carlo (" << sims << " simulations) ──────────────\n";
        auto row = [&](const char* l, double v, const char* fmt = "%.2f") {
            char buf[32]; std::snprintf(buf, sizeof(buf), fmt, v);
            ss << "  " << std::left << std::setw(28) << l << " $" << buf << "\n";
        };
        row("5th Pct Final NAV",   mc.p5_final_nav);
        row("25th Pct Final NAV",  mc.p25_final_nav);
        row("Median Final NAV",    mc.median_final_nav);
        row("75th Pct Final NAV",  mc.p75_final_nav);
        row("95th Pct Final NAV",  mc.p95_final_nav);
        ss << "  " << std::left << std::setw(28) << "95th Pct Max DD (%)"
           << "  " << std::fixed << std::setprecision(1) << mc.p95_max_drawdown << "\n";
        ss << "  " << std::left << std::setw(28) << "Prob Profitable (%)"
           << "  " << std::fixed << std::setprecision(1) << mc.prob_profitable << "\n";
        ss << "  " << std::left << std::setw(28) << "Median Sharpe"
           << "  " << std::fixed << std::setprecision(3) << mc.median_sharpe << "\n";
        ss << "────────────────────────────────────────────────\n";
        return ss.str();
    }

private:
    static std::vector<double> extract_returns(const std::vector<NAVPoint>& curve) {
        std::vector<double> rets;
        rets.reserve(curve.size());
        for (std::size_t i = 1; i < curve.size(); ++i) {
            double p = curve[i-1].nav;
            if (p > 0) rets.push_back((curve[i].nav - p) / p);
        }
        return rets;
    }

    static double mean(const std::vector<double>& v) {
        if (v.empty()) return 0;
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    }

    static double std_dev(const std::vector<double>& v) {
        if (v.size() < 2) return 0;
        double m = mean(v), var = 0;
        for (double x : v) var += (x-m)*(x-m);
        return std::sqrt(var/(v.size()-1));
    }

    static double sortino(const std::vector<double>& rets, double rf_d,
                           int bpy) {
        if (rets.size() < 2) return 0;
        double m = mean(rets) - rf_d;
        double dd = 0; int cnt = 0;
        for (double r : rets) if (r < rf_d) { dd += (r-rf_d)*(r-rf_d); ++cnt; }
        if (cnt < 2) return 0;
        double dsd = std::sqrt(dd / (cnt-1));
        return (dsd > 0) ? m / dsd * std::sqrt(bpy) : 0;
    }

    static double omega(const std::vector<double>& rets, double threshold) {
        double pos = 0, neg = 0;
        for (double r : rets) {
            if (r > threshold) pos += r - threshold;
            else               neg += threshold - r;
        }
        return (neg > 0) ? pos/neg : 9999;
    }

    static double ulcer(const std::vector<NAVPoint>& curve) {
        double peak = curve[0].nav, sum = 0;
        for (auto& p : curve) {
            peak = std::max(peak, p.nav);
            double dd = (peak > 0) ? (peak-p.nav)/peak * 100 : 0;
            sum += dd*dd;
        }
        return std::sqrt(sum/curve.size());
    }

    static std::tuple<double,double,double,double>
    drawdown_stats(const std::vector<NAVPoint>& curve) {
        double peak = curve[0].nav, mdd = 0, cur = 0;
        double sum_dd = 0, mdd_dur = 0, cur_dur = 0, sum_dur = 0;
        int dd_count = 0;
        for (auto& p : curve) {
            if (p.nav >= peak) { peak = p.nav; cur = 0; cur_dur = 0; }
            else {
                cur = (peak-p.nav)/peak;
                cur_dur++;
                mdd = std::max(mdd, cur);
                mdd_dur = std::max(mdd_dur, cur_dur);
                sum_dd += cur; ++dd_count;
                sum_dur += cur_dur;
            }
        }
        double avg_dd  = dd_count > 0 ? sum_dd/dd_count : 0;
        double avg_dur = dd_count > 0 ? sum_dur/dd_count : 0;
        return {mdd, mdd_dur, avg_dd, avg_dur};
    }

    static double beta_calc(const std::vector<double>& rets,
                              const std::vector<double>& bench) {
        int n = (int)std::min(rets.size(), bench.size());
        double cov = 0, var = 0, mr = mean(rets), mb = mean(bench);
        for (int i = 0; i < n; ++i) {
            cov += (rets[i]-mr)*(bench[i]-mb);
            var += (bench[i]-mb)*(bench[i]-mb);
        }
        return (var > 0) ? cov/var : 1.0;
    }

    static double pearson_corr(const std::vector<double>& a,
                                const std::vector<double>& b) {
        int n = (int)std::min(a.size(), b.size());
        double ma = mean(a), mb = mean(b), num = 0, va = 0, vb = 0;
        for (int i = 0; i < n; ++i) {
            num += (a[i]-ma)*(b[i]-mb);
            va  += (a[i]-ma)*(a[i]-ma);
            vb  += (b[i]-mb)*(b[i]-mb);
        }
        return (va*vb > 0) ? num/std::sqrt(va*vb) : 0;
    }

    static void up_down_capture(const std::vector<double>& rets,
                                 const std::vector<double>& bench,
                                 double& up, double& dn) {
        int n = (int)std::min(rets.size(), bench.size());
        double us = 0, ub = 0, ds = 0, db = 0;
        int uc = 0, dc = 0;
        for (int i = 0; i < n; ++i) {
            if (bench[i] > 0) { us += rets[i]; ub += bench[i]; ++uc; }
            else               { ds += rets[i]; db += bench[i]; ++dc; }
        }
        up = (uc > 0 && ub != 0) ? (us/uc)/(ub/uc)*100 : 0;
        dn = (dc > 0 && db != 0) ? (ds/dc)/(db/dc)*100 : 0;
    }
};

} // namespace analytics
} // namespace ql
