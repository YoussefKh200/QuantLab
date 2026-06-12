#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/analytics/BenchmarkComparison.h
// Side-by-side strategy vs benchmark tearsheet.
// Ported from QuantEngine v2, integrated with ql::analytics::Analytics.
//
// Provides:
//   BuyAndHoldStrategy     — passive benchmark
//   SixtyFortyStrategy     — classic 60/40 benchmark with periodic rebalance
//   BenchmarkStats         — alpha, beta, correlation, TE, IR, up/down capture
//   BenchmarkRunner        — runs strategy + benchmark, prints tearsheet
// ═══════════════════════════════════════════════════════════════════════════
#include "../simulation/Backtester.h"
#include "Analytics.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace ql {
namespace analytics {

// ── Passive buy-and-hold benchmark ────────────────────────────────────────
struct BHParams {
    InstrumentID instrument = 0;
    std::string  ticker;
    double       notional_pct = 0.95;  // invest 95% of initial capital
};

class BuyAndHoldStrategy : public IStrategy {
public:
    explicit BuyAndHoldStrategy(BHParams p = BHParams()) : p_(p) {}

    void on_start(MarketSimulator& sim, Portfolio& pf) override {
        IStrategy::on_start(sim, pf);
        // Resolve ticker → InstrumentID if not set
        if (p_.instrument == 0 && !p_.ticker.empty())
            p_.instrument = SymbolRegistry::instance().get_or_create(p_.ticker);
    }

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        if (bought_) return;
        if (p_.instrument != 0 && bar.instrument != p_.instrument) return;
        if (p_.instrument == 0) {
            // Buy first instrument we see
            p_.instrument = bar.instrument;
        }
        double notional = pf.initial_cash() * p_.notional_pct;
        double qty      = std::floor(notional / bar.close);
        if (qty > 0) { buy_market(bar.instrument, qty); bought_ = true; }
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        // Hold to the end — no exit
    }

private:
    BHParams p_;
    bool     bought_ = false;
};

// ── 60/40 benchmark ────────────────────────────────────────────────────────
struct SFParams {
    std::string  equity_ticker = "SPY";
    std::string  bond_ticker   = "TLT";
    double       equity_pct    = 0.60;
    double       bond_pct      = 0.40;
    double       notional_pct  = 0.95;
    int          rebal_freq    = 63;   // quarterly
};

class SixtyFortyStrategy : public IStrategy {
public:
    explicit SixtyFortyStrategy(SFParams p = SFParams()) : p_(p) {}

    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        last_prices_[bar.instrument] = bar.close;
        ++bar_count_;

        // Initial buy once both instruments seen
        if (!init_done_) {
            auto eq_id = SymbolRegistry::instance().find_id(p_.equity_ticker);
            auto bd_id = SymbolRegistry::instance().find_id(p_.bond_ticker);
            if (eq_id && bd_id &&
                last_prices_.count(*eq_id) && last_prices_.count(*bd_id)) {
                execute_rebal(pf);
                init_done_ = true;
            }
            return;
        }
        if (bar_count_ % p_.rebal_freq == 0) execute_rebal(pf);
    }

    void on_end(MarketSimulator& sim, Portfolio& pf) override {}

private:
    void execute_rebal(Portfolio& pf) {
        double total = pf.initial_cash() * p_.notional_pct;
        auto try_buy = [&](const std::string& ticker, double pct) {
            auto id_opt = SymbolRegistry::instance().find_id(ticker);
            if (!id_opt) return;
            auto it = last_prices_.find(*id_opt);
            if (it == last_prices_.end() || it->second <= 0) return;
            double qty = std::floor(total * pct / it->second);
            if (qty > 0) buy_market(*id_opt, qty);
        };
        try_buy(p_.equity_ticker, p_.equity_pct);
        try_buy(p_.bond_ticker,   p_.bond_pct);
    }

    SFParams p_;
    std::unordered_map<InstrumentID, double> last_prices_;
    int  bar_count_ = 0;
    bool init_done_ = false;
};

// ── Benchmark statistics ───────────────────────────────────────────────────
struct BenchmarkStats {
    double alpha             = 0;   // Jensen's alpha (annualised, %)
    double beta              = 0;
    double correlation       = 0;
    double tracking_error    = 0;   // annualised, %
    double information_ratio = 0;
    double up_capture        = 0;   // %
    double down_capture      = 0;   // %
    double active_return     = 0;   // annualised, %
};

inline BenchmarkStats compute_benchmark_stats(
    const std::vector<NAVPoint>& strat,
    const std::vector<NAVPoint>& bench,
    int bars_per_year = 252,
    double rf         = 0.05)
{
    BenchmarkStats bs;
    std::size_t n = std::min(strat.size(), bench.size());
    if (n < 10) return bs;

    auto make_rets = [](const std::vector<NAVPoint>& c, std::size_t len) {
        std::vector<double> r;
        r.reserve(len);
        for (std::size_t i = 1; i < len; ++i) {
            double p = c[i-1].nav;
            if (p > 0) r.push_back((c[i].nav - p) / p);
        }
        return r;
    };

    auto sr = make_rets(strat, n);
    auto br = make_rets(bench, n);
    std::size_t m = std::min(sr.size(), br.size());
    if (m < 5) return bs;
    sr.resize(m); br.resize(m);

    auto mean = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto stdv = [&mean](const std::vector<double>& v) {
        double mu = mean(v), s = 0;
        for (double x : v) s += (x-mu)*(x-mu);
        return std::sqrt(s / (v.size()-1));
    };

    double ms = mean(sr), mb = mean(br);
    double ss = stdv(sr), sb = stdv(br);

    // Covariance
    double cov = 0;
    for (std::size_t i = 0; i < m; ++i) cov += (sr[i]-ms)*(br[i]-mb);
    cov /= (m-1);

    bs.beta        = (sb*sb > 0) ? cov/(sb*sb) : 1.0;
    bs.correlation = (ss*sb > 0) ? cov/(ss*sb) : 0.0;

    // Jensen's alpha (annualised %)
    double ann_s = ms * bars_per_year * 100;
    double ann_b = mb * bars_per_year * 100;
    bs.alpha      = ann_s - (rf*100 + bs.beta*(ann_b - rf*100));
    bs.active_return = ann_s - ann_b;

    // Tracking error
    std::vector<double> active(m);
    for (std::size_t i = 0; i < m; ++i) active[i] = sr[i] - br[i];
    bs.tracking_error    = stdv(active) * std::sqrt(bars_per_year) * 100;
    double active_mean   = mean(active) * bars_per_year;
    bs.information_ratio = (bs.tracking_error > 0)
                           ? active_mean*100 / bs.tracking_error : 0;

    // Up/Down capture
    double us=0, ub=0, ds=0, db=0; int uc=0, dc=0;
    for (std::size_t i = 0; i < m; ++i) {
        if (br[i] > 0) { us+=sr[i]; ub+=br[i]; ++uc; }
        else           { ds+=sr[i]; db+=br[i]; ++dc; }
    }
    bs.up_capture   = (uc>0 && ub!=0) ? (us/uc)/(ub/uc)*100 : 0;
    bs.down_capture = (dc>0 && db!=0) ? (ds/dc)/(db/dc)*100 : 0;

    return bs;
}

// ── Comparison tearsheet ───────────────────────────────────────────────────
struct Tearsheet {
    PerformanceReport strategy_report;
    PerformanceReport benchmark_report;
    BenchmarkStats    relative;
    std::string       strategy_name;
    std::string       benchmark_name;
};

inline std::string format_tearsheet(const Tearsheet& ts) {
    std::ostringstream ss;
    int col_w = 16;

    auto hdr = [&](const char* lbl) {
        ss << "  " << std::left << std::setw(28) << lbl
           << std::right << std::setw(col_w) << ts.strategy_name
           << std::right << std::setw(col_w) << ts.benchmark_name << "\n";
    };
    auto row = [&](const char* lbl, double sv, double bv, const char* fmt="%+.2f") {
        char sb[32], bb[32];
        std::snprintf(sb, sizeof(sb), fmt, sv);
        std::snprintf(bb, sizeof(bb), fmt, bv);
        ss << "  " << std::left  << std::setw(28) << lbl
           << std::right << std::setw(col_w) << sb
           << std::right << std::setw(col_w) << bb << "\n";
    };
    auto row1 = [&](const char* lbl, double v, const char* fmt="%+.2f") {
        char buf[32]; std::snprintf(buf, sizeof(buf), fmt, v);
        ss << "  " << std::left << std::setw(28) << lbl
           << std::right << std::setw(col_w) << buf << "\n";
    };
    auto sep = [&](const char* s) {
        ss << "  -- " << s << " " << std::string(40-4-(int)strlen(s),'-') << "\n";
    };

    const auto& S = ts.strategy_report;
    const auto& B = ts.benchmark_report;
    const auto& R = ts.relative;

    ss << "\n";
    ss << "╔══════════════════════════════════════════════════════════╗\n";
    ss << "║                 PERFORMANCE TEARSHEET                    ║\n";
    ss << "╠══════════════════════════════════════════════════════════╣\n";

    hdr("Metric");
    sep("Returns");
    row("Total Return (%)",    S.total_return_pct,  B.total_return_pct);
    row("CAGR (%)",            S.cagr,              B.cagr);
    row("Ann. Volatility (%)", S.annualized_vol,    B.annualized_vol,  "%.2f");
    sep("Risk-Adjusted");
    row("Sharpe Ratio",        S.sharpe_ratio,      B.sharpe_ratio,    "%.3f");
    row("Sortino Ratio",       S.sortino_ratio,     B.sortino_ratio,   "%.3f");
    row("Calmar Ratio",        S.calmar_ratio,      B.calmar_ratio,    "%.3f");
    sep("Drawdown");
    row("Max Drawdown (%)",    S.max_drawdown,      B.max_drawdown,    "%.2f");
    row("Avg Drawdown (%)",    S.avg_drawdown,      B.avg_drawdown,    "%.2f");
    sep("Trades");
    row("Win Rate (%)",        S.win_rate,          B.win_rate,        "%.1f");
    row("Profit Factor",       S.profit_factor,     B.profit_factor,   "%.3f");
    row("Expectancy ($)",      S.expectancy,        B.expectancy,      "%.2f");
    sep("Relative to Benchmark");
    row1("Alpha (ann. %)",        R.alpha,                             "%.2f");
    row1("Beta",                  R.beta,                              "%.3f");
    row1("Correlation",           R.correlation,                       "%.3f");
    row1("Tracking Error (%)",    R.tracking_error,                    "%.2f");
    row1("Information Ratio",     R.information_ratio,                 "%.3f");
    row1("Up Capture (%)",        R.up_capture,                        "%.1f");
    row1("Down Capture (%)",      R.down_capture,                      "%.1f");
    row1("Active Return (ann. %)",R.active_return,                     "%.2f");

    ss << "╚══════════════════════════════════════════════════════════╝\n";
    return ss.str();
}

// ── Benchmark runner ───────────────────────────────────────────────────────
class BenchmarkRunner {
public:
    explicit BenchmarkRunner(BacktestConfig cfg = BacktestConfig()) : cfg_(cfg) {}

    Tearsheet run(
        std::vector<std::shared_ptr<DataFeed>>  feeds,
        std::shared_ptr<IStrategy>              strategy,
        const std::string&                      strategy_name,
        const std::string&                      benchmark_ticker,
        const std::string&                      benchmark_name = "Buy & Hold")
    {
        // Run strategy
        Backtester strat_bt(cfg_);
        for (auto& f : feeds) strat_bt.add_feed(f);
        strat_bt.add_strategy(strategy, strategy_name);
        auto strat_res = strat_bt.run();

        // Run buy-and-hold benchmark on same feeds
        BacktestConfig bench_cfg = cfg_;
        bench_cfg.verbose       = false;
        bench_cfg.run_monte_carlo = false;
        Backtester bench_bt(bench_cfg);
        for (auto& f : feeds) bench_bt.add_feed(f);

        BHParams bh;
        bh.ticker = benchmark_ticker;
        bench_bt.add_strategy(
            std::make_shared<BuyAndHoldStrategy>(bh), "benchmark");
        auto bench_res = bench_bt.run();

        // Compute relative stats
        auto rel = compute_benchmark_stats(
            strat_bt.portfolio().nav_curve(),
            bench_bt.portfolio().nav_curve(),
            cfg_.bars_per_year, cfg_.annual_rf);

        Tearsheet ts;
        ts.strategy_report  = strat_res.report;
        ts.benchmark_report = bench_res.report;
        ts.relative         = rel;
        ts.strategy_name    = strategy_name;
        ts.benchmark_name   = benchmark_name;

        if (cfg_.verbose) std::cout << format_tearsheet(ts);
        return ts;
    }

private:
    BacktestConfig cfg_;
};

} // namespace analytics
} // namespace ql
