// ═══════════════════════════════════════════════════════════════════════════
// QuantFusion v1 — Merged QuantEngine v2 + QuantLab v1
// main.cpp — 8 demonstrations covering all platform capabilities
// ═══════════════════════════════════════════════════════════════════════════
#include "src/core/Types.h"
#include "src/core/SymbolRegistry.h"
#include "src/core/Clock.h"
#include "src/data/DataManager.h"
#include "src/data/DataPipeline.h"
#include "src/simulation/Backtester.h"
#include "src/simulation/ExampleStrategies.h"
#include "src/simulation/RegimeDetection.h"
#include "src/simulation/PortfolioStrategy.h"
#include "src/factors/AlphaEngine.h"
#include "src/analytics/Analytics.h"
#include "src/analytics/BenchmarkComparison.h"
#include "src/portfolio/PortfolioConstruction.h"
#include "src/risk/RiskEngine.h"
#include "src/optimizer/Optimizer.h"
#include "src/microstructure/Microstructure.h"
#include "src/execution/OrderBook.h"
#include "src/regimes/RegimeEngine.h"
#include "src/alpha_factory/AlphaFactory.h"
#include "src/validation/WalkForwardValidator.h"
#include "src/validation/RobustnessEngine.h"
#include "src/ranking/StrategyRanker.h"
#include "src/ranking/StrategyRegistry.h"
#include "src/storage/StorageEngine.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace ql;

// ── Timer ──────────────────────────────────────────────────────────────────
struct Timer {
    std::chrono::high_resolution_clock::time_point t0 =
        std::chrono::high_resolution_clock::now();
    double ms() const {
        return std::chrono::duration<double,std::milli>(
            std::chrono::high_resolution_clock::now()-t0).count();
    }
};

static void section(const char* s) {
    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << std::left << std::setw(54) << s << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 1: Parallel DataPipeline → MA Crossover + Monte Carlo  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_pipeline_and_backtest() {
    section("DEMO 1: Parallel DataPipeline → MA Crossover + Monte Carlo");

    // Build 5 synthetic feeds in parallel via DataPipeline
    DataPipeline pipe;
    struct Asset { const char* t; double px; double vol; unsigned seed; };
    std::vector<Asset> assets = {
        {"SPY",450.,0.15,1001},{"QQQ",380.,0.22,2002},
        {"IWM",200.,0.20,3003},{"GLD",190.,0.13,4004},
        {"TLT",100.,0.12,5005}
    };
    Timestamp start = Clock::from_date(2019,1,2);
    for (auto& a : assets)
        pipe.add_synthetic(a.t, start, 1260, a.px, a.vol, 0.09, a.seed);

    std::printf("[Pipeline] %zu hardware threads\n", pipe.thread_count());
    Timer pt;
    auto results = pipe.run();
    std::printf("[Pipeline] Preprocessed %zu assets in %.1f ms\n",
                results.size(), pt.ms());

    // Print enriched stats for last bar of each asset
    std::printf("  %-8s  %7s  %6s  %5s  %6s\n","Asset","VWAP","ATR14","RSI14","BB%%");
    for (auto& r : results) {
        if (r.error.empty() && !r.enriched.empty()) {
            auto& e = r.enriched.back();
            std::printf("  %-8s  %7.2f  %6.2f  %5.1f  %6.2f\n",
                r.symbol.c_str(), e.vwap, e.atr14, e.rsi14, e.bb_pct*100);
        }
    }

    // Backtest: vol-targeted MA crossover on all 5 assets
    BacktestConfig cfg;
    cfg.initial_cash    = 1'000'000.0;
    cfg.annual_rf       = 0.05;
    cfg.run_monte_carlo = true;
    cfg.mc_simulations  = 1000;
    cfg.verbose         = true;
    cfg.slippage        = SlippageEngine(std::make_shared<SquareRootImpact>());

    Backtester bt(cfg);
    for (auto& r : results) if (r.feed) bt.add_feed(r.feed);

    MACrossParams mp;
    mp.fast=10; mp.slow=50; mp.notional=150000.0;
    mp.size_by_vol=true; mp.target_vol=0.12;
    bt.add_strategy(std::make_shared<MACrossStrategy>(mp), "MA_VolScaled");

    Timer bt_t;
    auto res = bt.run();
    std::printf("[Backtest] %.1f ms\n", bt_t.ms());
    bt.export_nav_csv("/tmp/qf_nav_demo1.csv");
    bt.export_trades_csv("/tmp/qf_trades_demo1.csv");
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 2: Regime-Filtered MA vs Buy-and-Hold Tearsheet  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_regime_vs_benchmark() {
    section("DEMO 2: Regime-Filtered Strategy vs Buy & Hold Tearsheet");

    // Generate data with pronounced bear regimes
    auto spy = data_mgr().generate_synthetic(
        "SPY_RG", Clock::from_date(2016,1,4), 1260,
        300., 0.18, 0.09, 7777);
    // Patch regime params for more pronounced cycles
    // (synthetic generator already uses regime switching)

    BacktestConfig cfg;
    cfg.initial_cash=1'000'000.0; cfg.annual_rf=0.05;
    cfg.verbose=true;
    cfg.slippage=SlippageEngine(std::make_shared<SpreadSlippage>());

    RFMAParams rp;
    rp.fast_period=10; rp.slow_period=50;
    rp.notional=900'000.0; rp.min_bull_prob=0.55;

    analytics::BenchmarkRunner runner(cfg);
    runner.run(
        {spy},
        std::make_shared<RegimeFilteredMAStrategy>(rp),
        "RegimeFilteredMA",
        "SPY_RG",
        "Buy & Hold SPY"
    );
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 3: Dynamic Risk-Parity Portfolio Strategy  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_portfolio_strategy() {
    section("DEMO 3: Dynamic Risk-Parity Portfolio — 6 Assets");

    struct A { const char* t; double px,v,r; unsigned s; };
    std::vector<A> assets = {
        {"SPY_RP",450.,0.15,0.10,101},{"QQQ_RP",380.,0.22,0.12,202},
        {"IWM_RP",200.,0.20,0.08,303},{"GLD_RP",190.,0.13,0.05,404},
        {"TLT_RP",100.,0.12,0.03,505},{"EEM_RP",45., 0.22,0.07,606},
    };

    BacktestConfig cfg;
    cfg.initial_cash=2'000'000.0; cfg.verbose=true;
    cfg.slippage=SlippageEngine(std::make_shared<VolumeParticipationSlippage>(0.10));
    cfg.risk_limits.max_drawdown_pct=0.25;

    Backtester bt(cfg);
    Timestamp start=Clock::from_date(2018,1,2);
    for (auto& a : assets)
        bt.add_feed(data_mgr().generate_synthetic(a.t,start,1260,a.px,a.v,a.r,a.s));

    PortfolioStrategyParams pp;
    pp.fast_period=10; pp.slow_period=50;
    pp.rebal_freq=21; pp.total_notional=1'800'000.0;
    pp.method=DynamicAllocMethod::RiskParity;
    bt.add_strategy(std::make_shared<PortfolioAwareStrategy>(pp), "RiskParity");
    bt.run();
    bt.export_nav_csv("/tmp/qf_nav_demo3.csv");
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 4: Multi-Strategy Portfolio (all 5 strategies on 3 assets)  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_multi_strategy() {
    section("DEMO 4: Multi-Strategy — 5 Strategies × 3 Assets");

    BacktestConfig cfg;
    cfg.initial_cash=3'000'000.0; cfg.verbose=true;

    Backtester bt(cfg);
    Timestamp start=Clock::from_date(2019,1,2);
    bt.add_feed(data_mgr().generate_synthetic("AAPL", start, 1260, 175.,0.28,0.12,111));
    bt.add_feed(data_mgr().generate_synthetic("MSFT", start, 1260, 380.,0.24,0.11,222));
    bt.add_feed(data_mgr().generate_synthetic("SPY",  start, 1260, 450.,0.15,0.09,333));

    MACrossParams     mp; mp.fast=10; mp.slow=50; mp.notional=200000.;
    MeanRevParams     mr; mr.lookback=20; mr.entry_z=-2.0; mr.notional=150000.;
    BreakoutParams    bp; bp.channel_period=20; bp.notional=150000.;
    VolTargetParams   vp; vp.notional=200000.; vp.target_vol=0.12;

    bt.add_strategy(std::make_shared<MACrossStrategy>(mp),          "MACross");
    bt.add_strategy(std::make_shared<MeanReversionStrategy>(mr),    "MeanRev");
    bt.add_strategy(std::make_shared<BreakoutStrategy>(bp),         "Breakout");
    bt.add_strategy(std::make_shared<VolTargetTrendStrategy>(vp),   "VolTarget");

    // Cross-sectional with 12-1 month momentum
    CrossSectionalParams cp; cp.rebal_freq=21; cp.gross_notional=600000.;
    auto factor_fn = [](InstrumentID, const std::deque<Bar>& bars) {
        if ((int)bars.size()<252) return 0.0;
        int n=(int)bars.size();
        double p12=bars[n-252].close, p1=bars[n-21].close;
        return (p12>0) ? std::log(p1/p12) : 0.0;
    };
    bt.add_strategy(
        std::make_shared<CrossSectionalStrategy>(cp, factor_fn), "XSection");

    bt.run();
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 5: Alpha Factor IC Research  (QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_alpha_research() {
    section("DEMO 5: Alpha Factor IC/ICIR Analysis — 8 Factors × 15 Assets");

    std::vector<std::shared_ptr<DataFeed>> feeds;
    Timestamp start=Clock::from_date(2015,1,2);
    for (int i=0; i<15; ++i) {
        char t[12]; std::snprintf(t,sizeof(t),"SIM%02d",i);
        feeds.push_back(data_mgr().generate_synthetic(
            t, start, 1800, 50.+i*8, 0.14+i*0.01, 0.05+i*0.004, 1000+i*37));
    }

    factors::AlphaEngine alpha;
    alpha.add_factor(factors::momentum(21));
    alpha.add_factor(factors::momentum_skip(252,21));
    alpha.add_factor(factors::reversal(5));
    alpha.add_factor(factors::realized_vol(20));
    alpha.add_factor(factors::low_vol(60));
    alpha.add_factor(factors::nearness_to_high(252));
    alpha.add_factor(factors::volume_trend(5,20));
    alpha.add_factor(factors::vwap_deviation(10));

    std::printf("\n  %-34s  %8s  %8s  %8s\n","Factor","Mean IC","IC Std","ICIR");
    std::printf("  %s\n",std::string(62,'-').c_str());

    Timestamp ts_start=Clock::from_date(2017,1,3);
    Timestamp ts_end  =Clock::from_date(2021,12,31);
    for (auto& f : alpha.factors()) {
        auto ic=alpha.compute_ic(f->name(),feeds,ts_start,ts_end,21,21);
        std::printf("  %-34s  %+8.4f  %8.4f  %+8.3f\n",
                    f->name().c_str(), ic.mean_ic, ic.ic_std, ic.icir);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 6: Walk-Forward Optimization (LHS)  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_walk_forward() {
    section("DEMO 6: Walk-Forward Optimization — LHS on MA Crossover");

    auto feed=data_mgr().generate_synthetic(
        "SPY_WF",Clock::from_date(2015,1,2),2000,400.,0.18,0.09,9999);

    BacktestConfig base; base.initial_cash=500'000.; base.verbose=false;

    auto eval=[&feed,&base](const optimizer::ParamSet& ps,
                             int bar_start, int bar_end) -> double {
        const auto& all=feed->bars();
        if (bar_end>(int)all.size()) bar_end=(int)all.size();
        if (bar_end-bar_start<60) return -99.;
        std::vector<Bar> sl(all.begin()+bar_start, all.begin()+bar_end);
        auto id=feed->instrument();
        auto tmp=std::make_shared<DataFeed>(id,BarResolution::D1,std::move(sl));
        BacktestConfig cfg=base;
        Backtester bt(cfg);
        bt.add_feed(tmp);
        MACrossParams p; p.fast=(int)ps.at("fast"); p.slow=(int)ps.at("slow");
        p.notional=400000.;
        bt.add_strategy(std::make_shared<MACrossStrategy>(p),"ma");
        try { return bt.run().report.sharpe_ratio; } catch(...) { return -99.; }
    };

    std::vector<optimizer::ParamDef> params={{"fast",5,30,true},{"slow",20,80,true}};
    optimizer::WFConfig wfc; wfc.is_bars=252; wfc.oos_bars=63; wfc.step_bars=63;

    using LHS=optimizer::LatinHypercubeSampler;
    optimizer::WalkForward<LHS> wf(params,eval,wfc);
    auto wins=wf.run((int)feed->bars().size());

    double ai=0,ao=0;
    for (auto& w:wins) { ai+=w.is_score; ao+=w.oos_score; }
    if (!wins.empty()) {
        std::printf("\n  Avg IS Sharpe  : %.3f\n", ai/wins.size());
        std::printf("  Avg OOS Sharpe : %.3f\n", ao/wins.size());
        std::printf("  Avg Efficiency : %.1f%%\n",
                    100.*ao/std::max(ai,0.001));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 7: Portfolio Construction Comparison  (QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_portfolio_construction() {
    section("DEMO 7: Portfolio Construction — 6 Methods × 5 Assets");

    std::vector<std::string> names={"SPY","TLT","GLD","IWM","EEM"};
    std::vector<double> vols={0.15,0.12,0.14,0.20,0.22};
    std::vector<double> mus ={0.10,0.04,0.05,0.09,0.08};
    std::vector<std::vector<double>> corr={
        {1.00,-0.25, 0.05, 0.85, 0.70},
        {-0.25,1.00,-0.10,-0.20,-0.15},
        {0.05,-0.10, 1.00, 0.10, 0.15},
        {0.85,-0.20, 0.10, 1.00, 0.65},
        {0.70,-0.15, 0.15, 0.65, 1.00},
    };
    int n=(int)names.size();
    construction::CovMatrix cov(n,std::vector<double>(n));
    for (int i=0;i<n;++i)
        for (int j=0;j<n;++j)
            cov[i][j]=corr[i][j]*vols[i]*vols[j];

    struct M { std::string name; construction::WeightVec w; };
    std::vector<M> methods={
        {"Equal Weight",    construction::equal_weight(n)},
        {"Inverse Vol",     construction::inverse_vol(cov)},
        {"Risk Parity",     construction::risk_parity(cov)},
        {"Min Variance",    construction::min_variance(cov)},
        {"Max Sharpe",      construction::max_sharpe(cov,mus,0.04)},
        {"HRP",             construction::hrp(cov)},
    };

    std::printf("\n  %-12s","Asset");
    for (auto& m:methods) std::printf("  %-12s",m.name.c_str());
    std::printf("\n  %s\n",std::string(12+methods.size()*14,'-').c_str());
    for (int i=0;i<n;++i) {
        std::printf("  %-12s",names[i].c_str());
        for (auto& m:methods) std::printf("  %11.1f%%",m.w[i]*100);
        std::printf("\n");
    }
    std::printf("\n  %-12s  %8s  %8s  %8s\n","Method","Ret%","Vol%","Sharpe");
    for (auto& m:methods) {
        double pr=construction::port_ret(m.w,mus)*100;
        double pv=std::sqrt(construction::port_var(m.w,cov))*100;
        std::printf("  %-12s  %7.2f%%  %7.2f%%  %7.3f\n",
                    m.name.c_str(),pr,pv,(pv>0)?(pr-4.)/pv:0.);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 8: Microstructure + Slippage + Tick Simulation  (QE+QL)
// ═══════════════════════════════════════════════════════════════════════════
void demo_microstructure_and_tick() {
    section("DEMO 8: Microstructure Analysis + Tick Simulation");

    auto feed=data_mgr().generate_synthetic(
        "SPY_MS",Clock::from_date(2020,1,2),500,300.,0.20,0.08,42);

    micro::MicrostructureEngine engine;
    std::printf("\n  %-12s  %9s  %9s  %9s  %6s\n",
                "Date","CS-Sprd%","Roll-Sprd","Amihud","VPIN");
    const auto& bars=feed->bars();
    for (int i=std::max(0,(int)bars.size()-5);i<(int)bars.size();++i) {
        auto snap=engine.update_bar(bars[i]);
        time_t t=bars[i].ts_open/NS_PER_SEC;
        char buf[16]; strftime(buf,sizeof(buf),"%Y-%m-%d",gmtime(&t));
        std::printf("  %-12s  %9.4f%%  %9.4f%%  %9.6f  %6.4f\n",
                    buf,snap.cs_spread*100,snap.roll_spread*100,
                    snap.amihud_illiquidity,snap.vpin);
    }

    // Slippage model comparison
    std::printf("\n  %-24s  %10s  %8s\n","Slippage Model","Cost($)","Bps");
    const Bar& sb=bars.back();
    std::vector<std::pair<std::string,std::shared_ptr<ISlippageModel>>> models={
        {"Fixed 2bps",          std::make_shared<FixedSlippage>(2.0)},
        {"Spread-Based",        std::make_shared<SpreadSlippage>()},
        {"Volume Participation",std::make_shared<VolumeParticipationSlippage>()},
        {"Sqrt Impact (AC)",    std::make_shared<SquareRootImpact>()},
        {"Two-Component",       std::make_shared<TwoComponentImpact>()},
    };
    for (auto&[name,model]:models) {
        SlippageContext ctx{sb,10000.,OrderSide::Buy,
                           sb.close,sb.close*0.0002,1e6,0.20,0.01};
        double c=model->compute(ctx);
        std::printf("  %-24s  %10.2f  %8.2f\n",
                    name.c_str(),c*10000.,c/sb.close*10000.);
    }

    // Intra-bar tick simulation
    std::printf("\n  Intra-bar tick simulation (last bar, 50 ticks):\n");
    SynthConfig sc; sc.n_ticks=50; sc.spread_bps=1.5;
    auto ticks=IntraBarSynthesizer::synthesize(sb,sc);
    double lo=ticks[0].price,hi=ticks[0].price;
    for (auto& tk:ticks) { lo=std::min(lo,tk.price); hi=std::max(hi,tk.price); }
    std::printf("  Bar OHLC: %.2f / %.2f / %.2f / %.2f\n",
                sb.open,sb.high,sb.low,sb.close);
    std::printf("  Tick range: %.2f – %.2f  (%.0f ticks)\n",lo,hi,(double)ticks.size());

    // Simulated order book fill estimate
    SimulatedOrderBook book;
    book.update(sb.close, sb.close*0.0002);
    auto est=book.estimate_market_fill(OrderSide::Buy, 5000.0);
    std::printf("  Book fill (5000 shares): avg=%.4f  impact=%.4f\n",
                est.avg_price, est.price_impact);
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 9: Multi-Regime Engine — 9-state classification on multi-asset universe
// ═══════════════════════════════════════════════════════════════════════════
void demo_regime_engine() {
    section("DEMO 9: Multi-Regime Engine — 9-State Classification");

    std::vector<std::shared_ptr<DataFeed>> feeds;
    Timestamp start = Clock::from_date(2018,1,2);
    struct A { const char* t; double v; double r; unsigned s; };
    std::vector<A> assets = {
        {"SPY_RE",0.16,0.09,1},{"QQQ_RE",0.22,0.12,2},{"IWM_RE",0.20,0.07,3},
        {"TLT_RE",0.12,0.03,4},{"GLD_RE",0.13,0.05,5},{"HYG_RE",0.10,0.04,6},
    };
    for (auto& a : assets)
        feeds.push_back(data_mgr().generate_synthetic(a.t,start,1260,100.,a.v,a.r,a.s));

    regimes::RegimeEngine engine;

    // Feed all bars in time order, snapshot every 21 bars (~monthly)
    int n = (int)feeds[0]->bars().size();
    for (int i=0;i<n;++i) {
        for (auto& f : feeds) engine.feed(f->bars()[i]);
        if (i%21==0 && i>60) {
            auto state = engine.snapshot(feeds[0]->bars()[i].ts_open);
            // Track per-bar return for regime perf (using SPY as proxy)
            if (i>0) {
                double prev=feeds[0]->bars()[i-1].close, cur=feeds[0]->bars()[i].close;
                engine.record_bar_return(std::log(cur/prev), state.dominant);
            }
        }
    }

    auto counts = engine.regime_bar_counts();
    std::printf("\n  %-18s  %8s  %8s\n","Regime","Snaps","Pct");
    int total=0; for(int c:counts) total+=c;
    for (int i=0;i<9;++i) {
        if (counts[i]==0) continue;
        std::printf("  %-18s  %8d  %7.1f%%\n",
            regimes::regime_name(static_cast<regimes::Regime>(i)),
            counts[i], 100.0*counts[i]/total);
    }

    auto last = engine.last();
    std::printf("\n  Latest snapshot:\n");
    std::printf("    Dominant regime: %s (confidence=%.3f)\n",
                regimes::regime_name(last.dominant), last.confidence);
    std::printf("    Vol short/long:  %.3f / %.3f  (ratio=%.2f)\n",
                last.vol_short, last.vol_long, last.vol_ratio);
    std::printf("    Trend strength:  %+.3f\n", last.trend_strength);
    std::printf("    Breadth score:   %+.3f\n", last.breadth_score);
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 10: Alpha Factory — automated generation, evaluation, ranking
// ═══════════════════════════════════════════════════════════════════════════
void demo_alpha_factory() {
    section("DEMO 10: Alpha Factory — Automated Generation & Ranking");

    std::vector<std::shared_ptr<DataFeed>> feeds;
    Timestamp start = Clock::from_date(2017,1,3);
    for (int i=0;i<6;++i) {
        char t[16]; std::snprintf(t,sizeof(t),"AF%02d",i);
        feeds.push_back(data_mgr().generate_synthetic(
            t, start, 1000, 80.+i*15, 0.15+i*0.02, 0.06+i*0.01, 3000+i*53));
    }

    alpha::AlphaFactory::Config cfg;
    cfg.mc_sims  = 100;
    cfg.parallel = true;

    alpha::AlphaFactory factory(cfg);

    // Use a smaller spec set for demo speed
    std::vector<alpha::AlphaSpec> specs = {
        {"ma_cross",  {{"fast",{5,10,20}},{"slow",{30,50}}}},
        {"reversion", {{"lookback",{5,10,20}}}},
        {"breakout",  {{"channel",{10,20}},{"atr_mult",{2.0}}}},
        {"momentum",  {{"lookback",{21,63}}}},
    };

    Timer t;
    factory.run(feeds, specs);
    std::printf("[AlphaFactory] Total time: %.1f ms\n", t.ms());
    std::printf("[AlphaFactory] %zu candidates evaluated\n", factory.size());

    factory.print_top(8);

    // Persist
    storage::FlatFileStorage db("/tmp/qf_research_db");
    std::vector<alpha::AlphaCandidate> all_candidates;
    for (auto id : factory.top((int)factory.size()))
        if (auto* c = factory.get(id)) all_candidates.push_back(*c);
    db.persist_alphas(all_candidates, "demo10");
    std::printf("[Storage] Persisted %zu alphas to %s\n",
                all_candidates.size(), db.root().c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 11: Walk-Forward Validator + Robustness Engine + Strategy Ranker
// ═══════════════════════════════════════════════════════════════════════════
void demo_validation_and_ranking() {
    section("DEMO 11: WF Validator + Robustness + Strategy Ranker");

    auto feed = data_mgr().generate_synthetic(
        "VAL_SPY", Clock::from_date(2014,1,2), 1800, 350.,0.17,0.09,55555);

    BacktestConfig base; base.initial_cash=500000.; base.verbose=false;

    // Eval function: run MA cross with given params on a feed slice
    validation::EvalFn eval = [&](const optimizer::ParamSet& ps,
                                   const std::vector<std::shared_ptr<DataFeed>>& fs)
        -> analytics::PerformanceReport {
        if (fs.empty() || fs[0]->size()<30) return {};
        BacktestConfig cfg=base;
        Backtester bt(cfg);
        for (auto& f:fs) bt.add_feed(f);
        MACrossParams p;
        p.fast=(int)ps.at("fast"); p.slow=(int)ps.at("slow"); p.notional=300000.;
        bt.add_strategy(std::make_shared<MACrossStrategy>(p),"ma");
        try { return bt.run().report; } catch(...) { return {}; }
    };

    // Slice function: build a sub-feed from bar range
    validation::DataSliceFn slicer = [](const std::vector<std::shared_ptr<DataFeed>>& fs,
                                         int start, int end)
        -> std::vector<std::shared_ptr<DataFeed>> {
        std::vector<std::shared_ptr<DataFeed>> out;
        for (auto& f : fs) {
            const auto& bars = f->bars();
            int e = std::min(end,(int)bars.size());
            if (start>=e) continue;
            std::vector<Bar> sl(bars.begin()+start, bars.begin()+e);
            out.push_back(std::make_shared<DataFeed>(f->instrument(), f->resolution(), std::move(sl)));
        }
        return out;
    };

    std::vector<optimizer::ParamDef> params = {{"fast",5,30,true},{"slow",20,80,true}};
    validation::ValidatorConfig vcfg;
    vcfg.mode=validation::WFMode::Rolling;
    vcfg.is_bars=252; vcfg.oos_bars=63; vcfg.step_bars=63;
    vcfg.verbose=true;

    validation::WalkForwardValidator validator(eval, slicer, params, vcfg);
    Timer t;
    auto wf_result = validator.run({feed}, optimizer::Objective::Sharpe);
    std::printf("[WF Validator] %.1f ms\n", t.ms());

    // Run full backtest for robustness testing
    BacktestConfig full_cfg=base; full_cfg.verbose=false;
    Backtester full_bt(full_cfg);
    full_bt.add_feed(feed);
    MACrossParams mp; mp.fast=10; mp.slow=50; mp.notional=300000.;
    full_bt.add_strategy(std::make_shared<MACrossStrategy>(mp),"ma_full");
    auto full_res = full_bt.run();

    // Robustness engine
    validation::RobustnessEngine::Config rcfg;
    rcfg.n_mc_sims = 300;
    validation::RobustnessEngine robust(rcfg);

    auto rerun_fn = [&](double slip_mult, double /*noise*/) -> BacktestResult {
        BacktestConfig c2 = base;
        c2.verbose=false;
        c2.slippage = SlippageEngine(std::make_shared<FixedSlippage>(2.0*slip_mult));
        Backtester bt2(c2);
        bt2.add_feed(feed);
        bt2.add_strategy(std::make_shared<MACrossStrategy>(mp),"ma_stress");
        return bt2.run();
    };

    auto rob_report = robust.run(full_res.nav_curve, full_res.trades, rerun_fn);
    rob_report.print();

    // Strategy ranker
    ranking::StrategyRanker ranker;
    ranking::StrategyEntry entry;
    entry.id   = "ma_cross_10_50";
    entry.name = "MA_10_50_SPY";
    entry.perf = full_res.report;
    entry.wf   = wf_result;
    entry.robustness = rob_report;
    entry.regime_bars[(int)regimes::Regime::Bull]  = 200;
    entry.regime_sharpe[(int)regimes::Regime::Bull]= full_res.report.sharpe_ratio*1.2;
    entry.regime_bars[(int)regimes::Regime::Bear]  = 100;
    entry.regime_sharpe[(int)regimes::Regime::Bear]= full_res.report.sharpe_ratio*0.3;
    for (std::size_t i=1;i<full_res.nav_curve.size();++i) {
        double p=full_res.nav_curve[i-1].nav;
        if(p>0) entry.returns.push_back((full_res.nav_curve[i].nav-p)/p);
    }
    ranker.add(std::move(entry));
    ranker.rank();
    ranker.print();

    storage::FlatFileStorage db("/tmp/qf_research_db");
    db.persist_walkforward(wf_result, "ma_cross_10_50");
    db.persist_rankings(ranker.all(), "demo11");
    std::printf("[Storage] Persisted WF + ranking to %s\n", db.root().c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 12: Strategy Registry — Lifecycle + Degradation Monitoring
// ═══════════════════════════════════════════════════════════════════════════
void demo_lifecycle_monitoring() {
    section("DEMO 12: Strategy Lifecycle & Degradation Monitor");

    ranking::StrategyRegistry registry;
    registry.register_strategy("strat_001", "MA_10_50_Live");
    registry.register_strategy("strat_002", "MeanRev_20_Live");

    // Promote strat_001 through lifecycle
    registry.transition("strat_001", ranking::LifecycleState::Validated, "passed WF validation");
    registry.transition("strat_001", ranking::LifecycleState::PaperTrading, "paper trading 30d");
    registry.transition("strat_001", ranking::LifecycleState::Live, "promoted to live");

    ranking::PerformanceBaseline baseline;
    baseline.sharpe = 1.2;
    baseline.mean_ic = 0.05;
    baseline.avg_slippage_bps = 2.0;
    baseline.best_regime = regimes::Regime::Bull;
    registry.set_baseline("strat_001", baseline);
    registry.set_allocation("strat_001", 0.15, 10'000'000.0);

    // strat_002 stays in research
    registry.set_allocation("strat_002", 0.0, 10'000'000.0);

    // Simulate live observations: strategy degrades over time
    std::mt19937_64 rng(123);
    std::normal_distribution<double> good_ret(0.0008, 0.01);  // healthy regime
    std::normal_distribution<double> bad_ret(-0.0010, 0.018); // degraded regime

    std::printf("\n  Simulating 100 live bars (degradation after bar 60)...\n");
    for (int i=0;i<100;++i) {
        double ret = (i < 60) ? good_ret(rng) : bad_ret(rng);
        double ic  = (i < 60) ? 0.05 + 0.01*((double)rng()/rng.max()-0.5)
                               : 0.01 + 0.01*((double)rng()/rng.max()-0.5);
        double slip= (i < 60) ? 2.0 + 0.5*((double)rng()/rng.max())
                               : 4.0 + 1.0*((double)rng()/rng.max());

        auto alerts = registry.observe("strat_001", ret, ic, slip, nullptr);
        for (auto& a : alerts) {
            if (i>=60 && i<65) {
                const char* sev = a.severity==ranking::AlertSeverity::Critical?"CRIT"
                                 : a.severity==ranking::AlertSeverity::Warning?"WARN":"INFO";
                std::printf("    [bar %3d][%s] %s: %s\n", i, sev, a.type.c_str(), a.message.c_str());
            }
        }
    }

    registry.print_status();

    auto dealloc = registry.recommend_deallocation();
    std::printf("\n  Recommended deallocation: ");
    if (dealloc.empty()) std::printf("(none)\n");
    else { for (auto& id:dealloc) std::printf("%s ", id.c_str()); std::printf("\n"); }

    storage::FlatFileStorage db("/tmp/qf_research_db");
    db.persist_lifecycle(registry);
    std::printf("[Storage] Persisted lifecycle events to %s\n", db.root().c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << " ██████╗ ██╗   ██╗ █████╗ ███╗  ██╗████████╗\n";
    std::cout << "██╔═══██╗██║   ██║██╔══██╗████╗ ██║╚══██╔══╝\n";
    std::cout << "██║  ███║██║   ██║███████║██╔██╗██║   ██║   \n";
    std::cout << "██║   ██║██║   ██║██╔══██║██║╚████║   ██║   \n";
    std::cout << "╚██████╔╝╚██████╔╝██║  ██║██║ ╚███║   ██║   \n";
    std::cout << " ╚═════╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚══╝   ╚═╝  \n";
    std::cout << " ██████╗ ██╗   ██╗███████╗██╗ ██████╗ ███╗  ██╗\n";
    std::cout << "██╔════╝ ██║   ██║██╔════╝██║██╔═══██╗████╗ ██║\n";
    std::cout << "█████╗   ██║   ██║███████╗██║██║   ██║██╔██╗██║\n";
    std::cout << "██╔══╝   ██║   ██║╚════██║██║██║   ██║██║╚████║\n";
    std::cout << "██║      ╚██████╔╝███████║██║╚██████╔╝██║ ╚███║\n";
    std::cout << "╚═╝       ╚═════╝ ╚══════╝╚═╝ ╚═════╝ ╚═╝  ╚══╝\n";
    std::cout << " QuantFusion v1 — QuantEngine v2 + QuantLab v1\n\n";

    // Warm up thread pool and registry
    global_pool();

    bool all = (argc < 2);
    std::string sel = (argc >= 2) ? argv[1] : "";

    if (all || sel=="1") demo_pipeline_and_backtest();
    if (all || sel=="2") demo_regime_vs_benchmark();
    if (all || sel=="3") demo_portfolio_strategy();
    if (all || sel=="4") demo_multi_strategy();
    if (all || sel=="5") demo_alpha_research();
    if (all || sel=="6") demo_walk_forward();
    if (all || sel=="7") demo_portfolio_construction();
    if (all || sel=="8") demo_microstructure_and_tick();
    if (all || sel=="9") demo_regime_engine();
    if (all || sel=="10") demo_alpha_factory();
    if (all || sel=="11") demo_validation_and_ranking();
    if (all || sel=="12") demo_lifecycle_monitoring();

    return 0;
}
