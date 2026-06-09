// ═══════════════════════════════════════════════════════════════════════════
// QuantLab — Institutional Quantitative Research Platform
// main.cpp — Comprehensive demonstration suite
// ═══════════════════════════════════════════════════════════════════════════
#include "src/core/Types.h"
#include "src/core/SymbolRegistry.h"
#include "src/core/Clock.h"
#include "src/data/DataManager.h"
#include "src/simulation/Backtester.h"
#include "src/simulation/ExampleStrategies.h"
#include "src/factors/AlphaEngine.h"
#include "src/analytics/Analytics.h"
#include "src/portfolio/PortfolioConstruction.h"
#include "src/optimizer/Optimizer.h"
#include "src/microstructure/Microstructure.h"
#include "src/risk/RiskEngine.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <cmath>

using namespace ql;

// ─────────────────────────────────────────────────────────────────────────
// Timing helper
// ─────────────────────────────────────────────────────────────────────────
struct Timer {
    std::chrono::high_resolution_clock::time_point t0 =
        std::chrono::high_resolution_clock::now();

    double ms() const {
        return std::chrono::duration<double,std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }
};

static void section(const char* title) {
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(52) << title << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 1: Single-Asset MA Crossover — SPY 5yr synthetic, Monte Carlo
// ═══════════════════════════════════════════════════════════════════════════
void demo_ma_crossover() {
    section("DEMO 1: MA Crossover — SPY+QQQ 5yr + Monte Carlo");

    // Generate synthetic data
    auto spy = data_mgr().generate_synthetic(
        "SPY", Clock::from_date(2019,1,2), 1260,
        450.0, 0.17, 0.10, 1001);
    auto qqq = data_mgr().generate_synthetic(
        "QQQ", Clock::from_date(2019,1,2), 1260,
        370.0, 0.22, 0.12, 2002);

    // Configure
    BacktestConfig cfg;
    cfg.initial_cash    = 1'000'000.0;
    cfg.annual_rf       = 0.05;
    cfg.run_monte_carlo = true;
    cfg.mc_simulations  = 1000;
    cfg.verbose         = true;
    cfg.slippage        = SlippageEngine(std::make_shared<SquareRootImpact>());

    Backtester bt(cfg);
    bt.add_feed(spy);
    bt.add_feed(qqq);

    MACrossParams p;
    p.fast = 10; p.slow = 50;
    p.notional = 400'000.0;
    p.size_by_vol = true;
    p.target_vol  = 0.15;

    bt.add_strategy(std::make_shared<MACrossStrategy>(p), "MA_VolTarget");

    Timer t;
    auto result = bt.run();
    std::printf("[Timing] %.1f ms, %zu bars\n", t.ms(),
                spy->size() + qqq->size());

    bt.export_nav_csv("/tmp/ql_nav_demo1.csv");
    bt.export_trades_csv("/tmp/ql_trades_demo1.csv");
    std::cout << "[Output] /tmp/ql_nav_demo1.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 2: Cross-Sectional Factor Strategy — 10-asset universe
// Long top momentum quintile, short bottom
// ═══════════════════════════════════════════════════════════════════════════
void demo_cross_sectional() {
    section("DEMO 2: Cross-Sectional Momentum — 10 Asset Universe");

    struct Asset { const char* ticker; double px; double vol; unsigned seed; };
    std::vector<Asset> assets = {
        {"AAPL", 175.0, 0.28, 101}, {"MSFT", 380.0, 0.24, 202},
        {"GOOGL",140.0, 0.26, 303}, {"AMZN", 185.0, 0.30, 404},
        {"META", 320.0, 0.32, 505}, {"NVDA", 450.0, 0.40, 606},
        {"TSLA", 250.0, 0.55, 707}, {"JPM",  155.0, 0.22, 808},
        {"SPY",  450.0, 0.15, 909}, {"GLD",  190.0, 0.13, 111},
    };

    BacktestConfig cfg;
    cfg.initial_cash = 2'000'000.0;
    cfg.verbose      = true;
    // Volume impact slippage for cross-sectional
    cfg.slippage = SlippageEngine(std::make_shared<VolumeParticipationSlippage>(0.15));

    Backtester bt(cfg);

    for (auto& a : assets) {
        auto feed = data_mgr().generate_synthetic(
            a.ticker, Clock::from_date(2019,1,2), 1260,
            a.px, a.vol, 0.09, a.seed);
        bt.add_feed(feed);
    }

    // 12-1 month momentum factor
    CrossSectionalParams csp;
    csp.rebal_freq    = 21;
    csp.long_pct      = 0.30;
    csp.short_pct     = 0.30;
    csp.gross_notional= 1'800'000.0;

    auto factor_fn = [](InstrumentID, const std::deque<Bar>& bars) -> double {
        if ((int)bars.size() < 252) return 0;
        int n = (int)bars.size();
        double p12m = bars[n-252].close;
        double p1m  = bars[n-21].close;
        if (p12m <= 0) return 0;
        return std::log(p1m / p12m);  // 12-1 month momentum
    };

    bt.add_strategy(
        std::make_shared<CrossSectionalStrategy>(csp, factor_fn),
        "Momentum_L/S");

    Timer t;
    bt.run();
    std::printf("[Timing] %.1f ms\n", t.ms());

    bt.export_nav_csv("/tmp/ql_nav_demo2.csv");
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 3: Walk-Forward Optimization of MA parameters
// ═══════════════════════════════════════════════════════════════════════════
void demo_walk_forward() {
    section("DEMO 3: Walk-Forward Optimization — MA Crossover");

    auto spy = data_mgr().generate_synthetic(
        "SPY_WF", Clock::from_date(2016,1,4), 2000,
        400.0, 0.18, 0.09, 9999);

    BacktestConfig base_cfg;
    base_cfg.initial_cash = 500'000.0;
    base_cfg.verbose      = false;

    // Objective: run a backtest on bar slice [start, end], return Sharpe
    auto eval = [&spy, &base_cfg](const optimizer::ParamSet& ps,
                                   int bar_start, int bar_end) -> double {
        const auto& all_bars = spy->bars();
        if (bar_end > (int)all_bars.size()) bar_end = (int)all_bars.size();
        if (bar_end <= bar_start + 60) return -999;

        std::vector<Bar> slice(all_bars.begin() + bar_start,
                               all_bars.begin() + bar_end);
        // Register temporary feed
        auto tmp_id = SymbolRegistry::instance().get_or_create("SPY_WF");
        auto tmp_feed = std::make_shared<DataFeed>(tmp_id, BarResolution::D1,
                                                    std::move(slice));
        BacktestConfig cfg = base_cfg;
        Backtester bt(cfg);
        bt.add_feed(tmp_feed);

        MACrossParams p;
        p.fast     = (int)ps.at("fast");
        p.slow     = (int)ps.at("slow");
        p.notional = 400'000.0;
        bt.add_strategy(std::make_shared<MACrossStrategy>(p), "ma");

        try {
            auto res = bt.run();
            return res.report.sharpe_ratio;
        } catch (...) { return -999; }
    };

    // Parameter space
    std::vector<optimizer::ParamDef> params = {
        {"fast",  5, 30, true},
        {"slow", 20, 80, true},
    };

    optimizer::WFConfig wfc;
    wfc.is_bars   = 252;
    wfc.oos_bars  = 63;
    wfc.step_bars = 63;

    using LHS = optimizer::LatinHypercubeSampler;

    optimizer::WalkForward<LHS> wf(params, eval, wfc);
    auto results = wf.run((int)spy->bars().size());

    std::cout << "\n── Walk-Forward Summary ──────────────────────────────\n";
    double avg_is=0, avg_oos=0;
    for (auto& w : results) {
        avg_is  += w.is_score;
        avg_oos += w.oos_score;
    }
    if (!results.empty()) {
        avg_is  /= results.size();
        avg_oos /= results.size();
        std::printf("  Avg IS Sharpe:  %.3f\n", avg_is);
        std::printf("  Avg OOS Sharpe: %.3f\n", avg_oos);
        std::printf("  Avg Efficiency: %.1f%%\n", 100*avg_oos/std::max(avg_is,0.001));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 4: Alpha Factor Research — IC analysis across 5 factors
// ═══════════════════════════════════════════════════════════════════════════
void demo_alpha_research() {
    section("DEMO 4: Alpha Factor IC Analysis — 5-Factor Study");

    // Generate 20 synthetic assets
    std::vector<std::shared_ptr<DataFeed>> feeds;
    for (int i = 0; i < 20; ++i) {
        char tick[16]; std::snprintf(tick, sizeof(tick), "SIM%02d", i);
        feeds.push_back(data_mgr().generate_synthetic(
            tick, Clock::from_date(2016,1,4), 1500,
            50.0 + i*10, 0.15 + i*0.01, 0.05 + i*0.005, 1000+i*37));
    }

    factors::AlphaEngine alpha;
    alpha.add_factor(factors::momentum(21));
    alpha.add_factor(factors::momentum_skip(252, 21));
    alpha.add_factor(factors::reversal(5));
    alpha.add_factor(factors::realized_vol(20));
    alpha.add_factor(factors::low_vol(60));
    alpha.add_factor(factors::nearness_to_high(252));

    std::cout << "\n── Factor IC Analysis (21-bar forward return) ──────────\n";
    std::printf("  %-30s  %8s  %8s  %8s\n",
                "Factor", "Mean IC", "IC Std", "ICIR");
    std::printf("  %s\n", std::string(60, '-').c_str());

    Timestamp start = Clock::from_date(2017,1,3);
    Timestamp end   = Clock::from_date(2021,12,31);

    for (auto& f : alpha.factors()) {
        auto ic = alpha.compute_ic(f->name(), feeds, start, end, 21, 21);
        std::printf("  %-30s  %+8.4f  %8.4f  %+8.3f\n",
                    f->name().c_str(), ic.mean_ic, ic.ic_std, ic.icir);
    }

    // Cross-sectional snapshot at a single date
    std::cout << "\n── Cross-Sectional Factor Scores (sample date) ────────\n";
    Timestamp snap_ts = Clock::from_date(2020,6,15);
    auto snapshot = alpha.compute(feeds, snap_ts, 300);

    if (snapshot.count("Momentum(21)")) {
        auto& fr = snapshot["Momentum(21)"];
        std::sort(fr.begin(), fr.end(),
                  [](auto& a, auto& b){ return a.score > b.score; });
        std::cout << "  Top 5 by 1M Momentum:\n";
        for (int i = 0; i < std::min(5,(int)fr.size()); ++i)
            std::printf("    %-12s  raw=%+.4f  pct=%.0f\n",
                        ticker(fr[i].instrument).c_str(),
                        fr[i].raw, fr[i].percentile);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 5: Portfolio Construction Comparison
//         Equal Weight vs Risk Parity vs HRP vs Max Sharpe
// ═══════════════════════════════════════════════════════════════════════════
void demo_portfolio_construction() {
    section("DEMO 5: Portfolio Construction — 5 Methods Compared");

    // Simulate 5 assets and compute covariance matrix
    std::vector<std::string> names = {"SPY","TLT","GLD","IWM","EEM"};
    std::vector<double>      vols  = {0.15, 0.12, 0.14, 0.20, 0.22};
    std::vector<double>      mus   = {0.10, 0.04, 0.05, 0.09, 0.08};
    // Correlations: equity-like cluster (SPY,IWM,EEM), bond (TLT), commodity (GLD)
    std::vector<std::vector<double>> corr = {
        {1.00, -0.25,  0.05,  0.85,  0.70},  // SPY
        {-0.25, 1.00, -0.10, -0.20, -0.15},  // TLT
        {0.05, -0.10,  1.00,  0.10,  0.15},  // GLD
        {0.85, -0.20,  0.10,  1.00,  0.65},  // IWM
        {0.70, -0.15,  0.15,  0.65,  1.00},  // EEM
    };
    int n = (int)names.size();

    // Build covariance matrix
    construction::CovMatrix cov(n, std::vector<double>(n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            cov[i][j] = corr[i][j] * vols[i] * vols[j];

    // Run all methods
    struct Method {
        std::string name;
        construction::WeightVec weights;
    };

    std::vector<Method> methods = {
        {"Equal Weight",      construction::equal_weight(n)},
        {"Inverse Vol",       construction::inverse_vol(cov)},
        {"Risk Parity",       construction::risk_parity(cov)},
        {"Min Variance",      construction::min_variance(cov)},
        {"Max Sharpe",        construction::max_sharpe(cov, mus, 0.04)},
        {"HRP",               construction::hrp(cov)},
    };

    std::cout << "\n";
    std::printf("  %-18s", "Asset");
    for (auto& m : methods) std::printf("  %-12s", m.name.c_str());
    std::cout << "\n  " << std::string(18 + methods.size()*14, '-') << "\n";

    for (int i = 0; i < n; ++i) {
        std::printf("  %-18s", names[i].c_str());
        for (auto& m : methods)
            std::printf("  %11.1f%%", m.weights[i]*100);
        std::cout << "\n";
    }

    // Portfolio metrics for each method
    std::cout << "\n  ── Portfolio-Level Metrics ──\n";
    std::printf("  %-18s  %8s  %8s  %8s\n", "Method", "Ret%", "Vol%", "Sharpe");
    for (auto& m : methods) {
        double pr = construction::port_ret(m.weights, mus) * 100;
        double pv = std::sqrt(construction::port_var(m.weights, cov)) * 100;
        double sh = (pv > 0) ? (pr-4.0)/pv : 0;
        std::printf("  %-18s  %7.2f%%  %7.2f%%  %7.3f\n",
                    m.name.c_str(), pr, pv, sh);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO 6: Microstructure Analysis
// ═══════════════════════════════════════════════════════════════════════════
void demo_microstructure() {
    section("DEMO 6: Market Microstructure Analysis");

    // Simulate a daily bar series and compute microstructure metrics
    auto feed = data_mgr().generate_synthetic(
        "SPY_MS", Clock::from_date(2020,1,2), 500,
        300.0, 0.20, 0.08, 42);

    micro::MicrostructureEngine engine;

    std::cout << "\n── Microstructure Metrics (last 5 bars) ────────────────\n";
    std::printf("  %-12s  %8s  %8s  %8s  %8s\n",
                "Date", "CS-Spread", "Roll-Sprd", "Amihud", "VPIN");

    const auto& bars = feed->bars();
    for (int i = std::max(0,(int)bars.size()-5); i < (int)bars.size(); ++i) {
        auto snap = engine.update_bar(bars[i]);
        time_t t = bars[i].ts_open / NS_PER_SEC;
        char buf[16]; strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t));
        std::printf("  %-12s  %8.4f%%  %8.4f%%  %8.6f  %8.4f\n",
                    buf,
                    snap.cs_spread * 100,
                    snap.roll_spread * 100,
                    snap.amihud_illiquidity,
                    snap.vpin);
    }

    std::cout << "\n── Slippage Model Comparison ──────────────────────────\n";
    const Bar& sample = bars.back();
    std::vector<std::pair<std::string, std::shared_ptr<ISlippageModel>>> models = {
        {"Fixed 2bps",         std::make_shared<FixedSlippage>(2.0)},
        {"Spread-Based",       std::make_shared<SpreadSlippage>()},
        {"Vol Participation",  std::make_shared<VolumeParticipationSlippage>()},
        {"Sqrt Impact",        std::make_shared<SquareRootImpact>()},
        {"Two-Component",      std::make_shared<TwoComponentImpact>()},
    };

    std::printf("  %-22s  %10s  %10s\n", "Model", "Cost ($)", "Cost (bps)");
    for (auto& [name, model] : models) {
        SlippageContext ctx{sample, 10000.0, OrderSide::Buy,
                           sample.close, sample.close*0.0002,
                           1e6, 0.20, 0.01};
        double cost = model->compute(ctx);
        std::printf("  %-22s  %10.2f  %10.2f\n",
                    name.c_str(), cost * 10000, cost/sample.close*10000);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << " ██████╗ ██╗   ██╗ █████╗ ███╗  ██╗████████╗██╗      █████╗ ██████╗ \n";
    std::cout << "██╔═══██╗██║   ██║██╔══██╗████╗ ██║╚══██╔══╝██║     ██╔══██╗██╔══██╗\n";
    std::cout << "██║   ██║██║   ██║███████║██╔██╗██║   ██║   ██║     ███████║██████╔╝\n";
    std::cout << "██║▄▄ ██║██║   ██║██╔══██║██║╚████║   ██║   ██║     ██╔══██║██╔══██╗\n";
    std::cout << "╚██████╔╝╚██████╔╝██║  ██║██║ ╚███║   ██║   ███████╗██║  ██║██████╔╝\n";
    std::cout << " ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚══╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═════╝ \n";
    std::cout << " Institutional Quantitative Research Platform v1.0\n\n";

    // Init thread pool and symbol registry
    global_pool();

    bool run_all = (argc < 2);
    std::string sel = (argc >= 2) ? argv[1] : "";

    if (run_all || sel == "1") demo_ma_crossover();
    if (run_all || sel == "2") demo_cross_sectional();
    if (run_all || sel == "3") demo_walk_forward();
    if (run_all || sel == "4") demo_alpha_research();
    if (run_all || sel == "5") demo_portfolio_construction();
    if (run_all || sel == "6") demo_microstructure();

    return 0;
}
