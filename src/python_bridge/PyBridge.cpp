// ═══════════════════════════════════════════════════════════════════════════
// src/python_bridge/PyBridge.cpp
// Pybind11 integration: exposes the C++ simulation core to Python.
//
// Build: cmake .. -DBUILD_PYTHON=ON  (requires pybind11 + Python dev headers)
// Usage from Python:
//
//   import quantfusion as qf
//   feed = qf.generate_synthetic("SPY", 0, 1260, 450.0, 0.17, 0.10, 1001)
//   cfg  = qf.BacktestConfig()
//   cfg.initial_cash = 1_000_000
//   bt = qf.Backtester(cfg)
//   bt.add_feed(feed)
//   bt.add_ma_cross_strategy(fast=10, slow=50, notional=200_000)
//   result = bt.run()
//   print(result.sharpe_ratio, result.cagr, result.max_drawdown)
//
// Design notes:
//   - Heavy compute (backtests, optimization, Monte Carlo) stays in C++.
//   - Python receives plain dicts / numpy-friendly vectors for analysis,
//     not live C++ object references, to keep the boundary simple and
//     avoid lifetime issues.
//   - Returns NAV curves and trade lists as lists of dicts — directly
//     convertible to pandas.DataFrame via pd.DataFrame(records).
//   - For zero-copy transfer of large arrays (tick data, factor matrices),
//     use py::array_t<double> with buffer protocol (see export_factor_matrix).
// ═══════════════════════════════════════════════════════════════════════════
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <pybind11/functional.h>

#include "../core/Types.h"
#include "../core/SymbolRegistry.h"
#include "../core/Clock.h"
#include "../data/DataManager.h"
#include "../simulation/Backtester.h"
#include "../simulation/ExampleStrategies.h"
#include "../simulation/RegimeDetection.h"
#include "../simulation/PortfolioStrategy.h"
#include "../factors/AlphaEngine.h"
#include "../analytics/Analytics.h"
#include "../alpha_factory/AlphaFactory.h"
#include "../validation/WalkForwardValidator.h"
#include "../ranking/StrategyRanker.h"
#include "../regimes/RegimeEngine.h"
#include "../portfolio/PortfolioConstruction.h"

namespace py = pybind11;
using namespace ql;

// ── Helper: convert NAV curve to list of dicts ─────────────────────────────
static py::list nav_curve_to_pylist(const std::vector<NAVPoint>& curve) {
    py::list out;
    for (auto& p : curve) {
        py::dict d;
        d["timestamp"]       = p.ts;
        d["nav"]             = p.nav;
        d["cash"]            = p.cash;
        d["gross_exposure"]  = p.gross_exposure;
        d["net_exposure"]    = p.net_exposure;
        d["leverage"]        = p.leverage;
        d["daily_pnl"]       = p.daily_pnl;
        d["cum_pnl"]         = p.cum_pnl;
        d["drawdown"]        = p.drawdown;
        out.append(d);
    }
    return out;
}

// ── Helper: convert trade records to list of dicts ─────────────────────────
static py::list trades_to_pylist(const std::vector<TradeRecord>& trades) {
    py::list out;
    for (auto& t : trades) {
        py::dict d;
        d["instrument"]  = ticker(t.instrument);
        d["strategy_id"] = t.strategy_id;
        d["side"]        = (t.side == OrderSide::Buy) ? "LONG" : "SHORT";
        d["qty"]         = t.qty;
        d["entry_price"] = t.entry_price;
        d["exit_price"]  = t.exit_price;
        d["entry_ts"]    = t.entry_ts;
        d["exit_ts"]     = t.exit_ts;
        d["pnl"]         = t.pnl;
        d["commission"]  = t.commission;
        d["slippage"]    = t.slippage;
        out.append(d);
    }
    return out;
}

// ── Helper: convert factor cross-section to numpy-friendly dict ────────────
static py::dict factor_result_to_pydict(const factors::FactorResult& fr) {
    py::dict out;
    py::list tickers, raw, score, pct;
    for (auto& fv : fr) {
        tickers.append(ticker(fv.instrument));
        raw.append(fv.raw);
        score.append(fv.score);
        pct.append(fv.percentile);
    }
    out["ticker"]     = tickers;
    out["raw"]        = raw;
    out["score"]      = score;
    out["percentile"] = pct;
    return out;
}

// ── Helper: PerformanceReport → dict ────────────────────────────────────────
static py::dict report_to_pydict(const analytics::PerformanceReport& r) {
    py::dict d;
    d["total_return_pct"]  = r.total_return_pct;
    d["cagr"]              = r.cagr;
    d["annualized_vol"]    = r.annualized_vol;
    d["sharpe_ratio"]      = r.sharpe_ratio;
    d["sortino_ratio"]     = r.sortino_ratio;
    d["calmar_ratio"]      = r.calmar_ratio;
    d["max_drawdown"]      = r.max_drawdown;
    d["win_rate"]          = r.win_rate;
    d["profit_factor"]     = r.profit_factor;
    d["total_trades"]      = r.total_trades;
    d["alpha"]             = r.alpha;
    d["beta"]              = r.beta;
    d["information_ratio"] = r.information_ratio;
    return d;
}

// ── Python-friendly Backtester wrapper ─────────────────────────────────────
// Wraps ql::Backtester with simplified strategy-add methods (avoids exposing
// the full C++ template/inheritance machinery to Python).
class PyBacktester {
public:
    explicit PyBacktester(BacktestConfig cfg = BacktestConfig()) : bt_(cfg) {}

    void add_feed(std::shared_ptr<DataFeed> feed) { bt_.add_feed(feed); }

    void add_ma_cross(int fast, int slow, double notional,
                      bool allow_short=false, bool size_by_vol=false,
                      double target_vol=0.10) {
        MACrossParams p;
        p.fast=fast; p.slow=slow; p.notional=notional;
        p.allow_short=allow_short; p.size_by_vol=size_by_vol; p.target_vol=target_vol;
        bt_.add_strategy(std::make_shared<MACrossStrategy>(p), "ma_cross");
    }

    void add_mean_reversion(int lookback, double entry_z, double exit_z,
                            double notional) {
        MeanRevParams p;
        p.lookback=lookback; p.entry_z=entry_z; p.exit_z=exit_z; p.notional=notional;
        bt_.add_strategy(std::make_shared<MeanReversionStrategy>(p), "mean_rev");
    }

    void add_breakout(int channel, double atr_mult, double notional) {
        BreakoutParams p;
        p.channel_period=channel; p.atr_mult=atr_mult; p.notional=notional;
        bt_.add_strategy(std::make_shared<BreakoutStrategy>(p), "breakout");
    }

    void add_vol_target(int fast, int slow, double target_vol, double notional) {
        VolTargetParams p;
        p.fast=fast; p.slow=slow; p.target_vol=target_vol; p.notional=notional;
        bt_.add_strategy(std::make_shared<VolTargetTrendStrategy>(p), "vol_target");
    }

    void add_regime_filtered_ma(int fast, int slow, double notional,
                                 double min_bull_prob) {
        RFMAParams p;
        p.fast_period=fast; p.slow_period=slow; p.notional=notional;
        p.min_bull_prob=min_bull_prob;
        bt_.add_strategy(std::make_shared<RegimeFilteredMAStrategy>(p), "regime_ma");
    }

    py::dict run() {
        auto res = bt_.run();
        py::dict out;
        out["report"]     = report_to_pydict(res.report);
        out["nav_curve"]  = nav_curve_to_pylist(res.nav_curve);
        out["trades"]     = trades_to_pylist(res.trades);
        out["mc_median_sharpe"]    = res.mc.median_sharpe;
        out["mc_p5_sharpe"]        = res.mc.p5_sharpe;
        out["mc_prob_profitable"]  = res.mc.prob_profitable;
        return out;
    }

    const Portfolio& portfolio() const { return bt_.portfolio(); }

private:
    Backtester bt_;
};

// ── Python module definition ────────────────────────────────────────────────
PYBIND11_MODULE(quantfusion, m) {
    m.doc() = "QuantFusion C++20 simulation core — Python bindings";

    // ── Core types ────────────────────────────────────────────────────────
    py::enum_<BarResolution>(m, "BarResolution")
        .value("D1", BarResolution::D1)
        .value("H1", BarResolution::H1)
        .value("M1", BarResolution::M1)
        .value("M5", BarResolution::M5);

    py::class_<DataFeed, std::shared_ptr<DataFeed>>(m, "DataFeed")
        .def("size",   &DataFeed::size)
        .def("start",  &DataFeed::start)
        .def("end",    &DataFeed::end);

    // ── Synthetic data generation ───────────────────────────────────────────
    m.def("generate_synthetic",
        [](const std::string& ticker, Timestamp start, int n_bars,
           double price, double vol, double ret, unsigned seed) {
            return data_mgr().generate_synthetic(ticker, start, n_bars,
                                                  price, vol, ret, seed);
        },
        py::arg("ticker"), py::arg("start_ts"), py::arg("n_bars"),
        py::arg("price")=100.0, py::arg("vol")=0.20, py::arg("ret")=0.10,
        py::arg("seed")=42,
        "Generate a synthetic DataFeed (GBM + regime switching)");

    m.def("from_date", &Clock::from_date,
        py::arg("year"), py::arg("month"), py::arg("day"),
        py::arg("hour")=0, py::arg("min")=0, py::arg("sec")=0,
        "Convert a calendar date to a nanosecond Timestamp");

    // ── Backtest config ──────────────────────────────────────────────────────
    py::class_<BacktestConfig>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("initial_cash",    &BacktestConfig::initial_cash)
        .def_readwrite("annual_rf",       &BacktestConfig::annual_rf)
        .def_readwrite("bars_per_year",   &BacktestConfig::bars_per_year)
        .def_readwrite("verbose",         &BacktestConfig::verbose)
        .def_readwrite("run_monte_carlo", &BacktestConfig::run_monte_carlo)
        .def_readwrite("mc_simulations",  &BacktestConfig::mc_simulations);

    // ── Backtester ────────────────────────────────────────────────────────────
    py::class_<PyBacktester>(m, "Backtester")
        .def(py::init<BacktestConfig>(), py::arg("config")=BacktestConfig())
        .def("add_feed",                &PyBacktester::add_feed)
        .def("add_ma_cross",            &PyBacktester::add_ma_cross,
             py::arg("fast")=10, py::arg("slow")=50, py::arg("notional")=100000.0,
             py::arg("allow_short")=false, py::arg("size_by_vol")=false,
             py::arg("target_vol")=0.10)
        .def("add_mean_reversion",      &PyBacktester::add_mean_reversion,
             py::arg("lookback")=20, py::arg("entry_z")=-2.0,
             py::arg("exit_z")=0.0, py::arg("notional")=80000.0)
        .def("add_breakout",            &PyBacktester::add_breakout,
             py::arg("channel")=20, py::arg("atr_mult")=2.0, py::arg("notional")=80000.0)
        .def("add_vol_target",          &PyBacktester::add_vol_target,
             py::arg("fast")=20, py::arg("slow")=100,
             py::arg("target_vol")=0.10, py::arg("notional")=900000.0)
        .def("add_regime_filtered_ma",  &PyBacktester::add_regime_filtered_ma,
             py::arg("fast")=10, py::arg("slow")=50, py::arg("notional")=100000.0,
             py::arg("min_bull_prob")=0.55)
        .def("run", &PyBacktester::run,
             "Run the backtest. Returns dict with 'report', 'nav_curve', 'trades'");

    // ── Alpha factory ────────────────────────────────────────────────────────
    py::class_<alpha::AlphaCandidate>(m, "AlphaCandidate")
        .def_readonly("id",            &alpha::AlphaCandidate::id)
        .def_readonly("name",          &alpha::AlphaCandidate::name)
        .def_readonly("factor_type",   &alpha::AlphaCandidate::factor_type)
        .def_readonly("sharpe_is",     &alpha::AlphaCandidate::sharpe_is)
        .def_readonly("icir",          &alpha::AlphaCandidate::icir)
        .def_readonly("max_drawdown",  &alpha::AlphaCandidate::max_drawdown)
        .def_readonly("composite_score", &alpha::AlphaCandidate::composite_score)
        .def_readonly("rank",          &alpha::AlphaCandidate::rank);

    py::class_<alpha::AlphaFactory::Config>(m, "AlphaFactoryConfig")
        .def(py::init<>())
        .def_readwrite("mc_sims", &alpha::AlphaFactory::Config::mc_sims)
        .def_readwrite("parallel", &alpha::AlphaFactory::Config::parallel);

    py::class_<alpha::AlphaFactory>(m, "AlphaFactory")
        .def(py::init<alpha::AlphaFactory::Config>(),
             py::arg("config")=alpha::AlphaFactory::Config())
        .def("run", [](alpha::AlphaFactory& self,
                       std::vector<std::shared_ptr<DataFeed>> feeds) {
                self.run(feeds);
            })
        .def("top", &alpha::AlphaFactory::top, py::arg("n")=20)
        .def("get", &alpha::AlphaFactory::get, py::return_value_policy::reference)
        .def("size", &alpha::AlphaFactory::size)
        .def("print_top", &alpha::AlphaFactory::print_top, py::arg("n")=10);

    // ── Regime engine ────────────────────────────────────────────────────────
    py::enum_<regimes::Regime>(m, "Regime")
        .value("Bull", regimes::Regime::Bull)
        .value("Bear", regimes::Regime::Bear)
        .value("Range", regimes::Regime::Range)
        .value("VolExpansion", regimes::Regime::VolExpansion)
        .value("VolCompression", regimes::Regime::VolCompression)
        .value("RiskOn", regimes::Regime::RiskOn)
        .value("RiskOff", regimes::Regime::RiskOff)
        .value("LiquidityCrisis", regimes::Regime::LiquidityCrisis)
        .value("MacroShock", regimes::Regime::MacroShock)
        .value("Unknown", regimes::Regime::Unknown);

    m.def("regime_name", &regimes::regime_name);

    py::class_<regimes::RegimeState>(m, "RegimeState")
        .def_readonly("dominant",     &regimes::RegimeState::dominant)
        .def_readonly("confidence",   &regimes::RegimeState::confidence)
        .def_readonly("vol_short",    &regimes::RegimeState::vol_short)
        .def_readonly("vol_long",     &regimes::RegimeState::vol_long)
        .def_readonly("trend_strength", &regimes::RegimeState::trend_strength)
        .def_readonly("breadth_score",  &regimes::RegimeState::breadth_score);

    py::class_<regimes::RegimeEngine>(m, "RegimeEngine")
        .def(py::init<>())
        .def("feed",     &regimes::RegimeEngine::feed)
        .def("snapshot", &regimes::RegimeEngine::snapshot);

    // ── Portfolio construction ──────────────────────────────────────────────
    m.def("equal_weight",  &construction::equal_weight);
    m.def("inverse_vol",   &construction::inverse_vol);
    m.def("risk_parity",   &construction::risk_parity, py::arg("cov"), py::arg("max_iter")=300);
    m.def("hrp",           &construction::hrp);
    m.def("min_variance",  &construction::min_variance,
          py::arg("cov"), py::arg("max_iter")=500, py::arg("lr")=0.01);
    m.def("max_sharpe",    &construction::max_sharpe,
          py::arg("cov"), py::arg("mu"), py::arg("rf")=0.05, py::arg("max_iter")=500);

    // ── Factor IC research ───────────────────────────────────────────────────
    using ICTS = factors::AlphaEngine::ICTimeSeries;
    py::class_<ICTS>(m, "ICTimeSeries")
        .def_readonly("dates",   &ICTS::dates)
        .def_readonly("ic",      &ICTS::ic)
        .def_readonly("mean_ic", &ICTS::mean_ic)
        .def_readonly("ic_std",  &ICTS::ic_std)
        .def_readonly("icir",    &ICTS::icir);
}
