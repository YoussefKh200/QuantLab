#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/Backtester.h
// Core backtest orchestrator.
//
// Wires: DataManager → EventBus → MarketSimulator → Portfolio → RiskEngine
//        → Strategy → Analytics
//
// Modes: Single strategy, Multi-strategy, Cross-sectional factor
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/Clock.h"
#include "../core/SymbolRegistry.h"
#include "../data/DataManager.h"
#include "../events/EventBus.h"
#include "MarketSimulator.h"
#include "../portfolio/Portfolio.h"
#include "../risk/RiskEngine.h"
#include "../analytics/Analytics.h"
#include "../execution/SlippageEngine.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <iostream>
#include <unordered_map>

namespace ql {

// ── Strategy interface ─────────────────────────────────────────────────────
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Called once before simulation starts
    virtual void on_start(MarketSimulator& sim, Portfolio& pf) {
        sim_ = &sim; pf_ = &pf;
    }

    // Called on every bar for subscribed instruments
    virtual void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) = 0;

    // Called on every fill directed to this strategy
    virtual void on_fill(const Fill& fill) {}

    // Called on risk events (halt signals, etc.)
    virtual void on_risk(const RiskEvent& ev) {}

    // Called once after last bar — close positions
    virtual void on_end(MarketSimulator& sim, Portfolio& pf) {}

    void         set_id(StrategyID id)     { id_ = id; }
    StrategyID   id()      const noexcept  { return id_; }
    void         set_name(std::string n)   { name_ = std::move(n); }
    const std::string& name() const        { return name_; }

protected:
    // ── Order helpers ──────────────────────────────────────────────────────
    OrderID buy_market(InstrumentID inst, double qty) {
        Order o; o.instrument=inst; o.side=OrderSide::Buy;
        o.type=OrderType::Market; o.quantity=qty; o.strategy_id=id_;
        return sim_->submit(o);
    }
    OrderID sell_market(InstrumentID inst, double qty) {
        Order o; o.instrument=inst; o.side=OrderSide::Sell;
        o.type=OrderType::Market; o.quantity=qty; o.strategy_id=id_;
        return sim_->submit(o);
    }
    OrderID buy_limit(InstrumentID inst, double qty, double px) {
        Order o; o.instrument=inst; o.side=OrderSide::Buy;
        o.type=OrderType::Limit; o.quantity=qty; o.limit_price=px; o.strategy_id=id_;
        return sim_->submit(o);
    }
    OrderID sell_limit(InstrumentID inst, double qty, double px) {
        Order o; o.instrument=inst; o.side=OrderSide::Sell;
        o.type=OrderType::Limit; o.quantity=qty; o.limit_price=px; o.strategy_id=id_;
        return sim_->submit(o);
    }
    OrderID sell_stop(InstrumentID inst, double qty, double stop) {
        Order o; o.instrument=inst; o.side=OrderSide::Sell;
        o.type=OrderType::Stop; o.quantity=qty; o.stop_price=stop; o.strategy_id=id_;
        return sim_->submit(o);
    }
    bool cancel_order(OrderID oid, InstrumentID inst) {
        return sim_->cancel(oid, inst);
    }

    // Portfolio read access
    double position_qty(InstrumentID inst) const {
        auto* pos = pf_->position(inst);
        return pos ? pos->quantity : 0.0;
    }
    double cash() const { return pf_->cash(); }
    double nav()  const { return pf_->current_nav(); }

    MarketSimulator* sim_ = nullptr;
    Portfolio*       pf_  = nullptr;
    StrategyID       id_  = 0;
    std::string      name_;
};

// ── Backtest configuration ─────────────────────────────────────────────────
struct BacktestConfig {
    double initial_cash    = 1'000'000.0;
    double annual_rf       = 0.05;
    int    bars_per_year   = 252;
    bool   verbose         = true;
    bool   run_monte_carlo = false;
    int    mc_simulations  = 2000;
    int    snapshot_freq   = 1;   // NAV snapshot every N bars

    SlippageEngine slippage;
    risk::RiskLimits risk_limits;
};

// ── Backtest result ────────────────────────────────────────────────────────
struct BacktestResult {
    analytics::PerformanceReport report;
    analytics::MonteCarloResult  mc;
    std::vector<NAVPoint>        nav_curve;
    std::vector<TradeRecord>     trades;
};

// ── Main backtest engine ───────────────────────────────────────────────────
class Backtester {
public:
    explicit Backtester(BacktestConfig cfg = {})
        : cfg_(cfg)
        , portfolio_(cfg.initial_cash)
        , risk_(cfg.risk_limits)
        , bus_()
        , sim_(bus_, cfg.slippage)
    {
        // Wire fill events → portfolio + strategy routing
        bus_.subscribe<FillEvent>([this](const FillEvent& ev) {
            portfolio_.on_fill(ev.fill);
            auto it = strategy_map_.find(ev.fill.strategy_id);
            if (it != strategy_map_.end())
                it->second->on_fill(ev.fill);
        });

        bus_.subscribe<RiskEvent>([this](const RiskEvent& ev) {
            for (auto& s : strategies_) s->on_risk(ev);
        });
    }

    // ── Data ───────────────────────────────────────────────────────────────
    void add_feed(std::shared_ptr<DataFeed> feed) {
        feeds_.push_back(std::move(feed));
    }

    void add_feeds(std::vector<std::shared_ptr<DataFeed>> feeds) {
        for (auto& f : feeds) feeds_.push_back(std::move(f));
    }

    // ── Strategy ───────────────────────────────────────────────────────────
    void add_strategy(std::shared_ptr<IStrategy> strat, std::string name = "") {
        static StrategyID next_id = 1;
        StrategyID id = next_id++;
        if (name.empty()) name = "strat_" + std::to_string(id);
        strat->set_id(id);
        strat->set_name(name);
        strategy_map_[id] = strat;
        strategies_.push_back(strat);
    }

    // ── Run ────────────────────────────────────────────────────────────────
    BacktestResult run() {
        if (feeds_.empty())   throw std::runtime_error("No data feeds");
        if (strategies_.empty()) throw std::runtime_error("No strategies");

        // Set simulation mode
        Clock::instance().set_mode(ClockMode::Simulation);

        // Find global time range
        Timestamp global_start = std::numeric_limits<Timestamp>::max();
        Timestamp global_end   = 0;
        for (auto& f : feeds_) {
            global_start = std::min(global_start, f->start());
            global_end   = std::max(global_end,   f->end());
        }
        Clock::instance().reset(global_start);

        // Build merged timeline
        struct BarRef { Timestamp ts; DataFeed* feed; std::size_t idx; };
        std::vector<BarRef> timeline;
        for (auto& feed : feeds_) {
            for (std::size_t i = 0; i < feed->size(); ++i)
                timeline.push_back({feed->bars()[i].ts_open, feed.get(), i});
        }
        std::sort(timeline.begin(), timeline.end(),
                  [](const BarRef& a, const BarRef& b){ return a.ts < b.ts; });

        // Start strategies
        for (auto& s : strategies_) s->on_start(sim_, portfolio_);

        // ── Main event loop ────────────────────────────────────────────────
        std::size_t bar_count = 0;
        Timestamp   last_snap_ts = 0;

        for (auto& ref : timeline) {
            const Bar& bar = ref.feed->bars()[ref.idx];
            Clock::instance().advance_to(bar.ts_open);

            // 1. Execution: attempt fills on this bar
            sim_.on_bar(bar);

            // 2. Mark-to-market
            portfolio_.mark_to_market(bar.instrument, bar.close);

            // 3. Dispatch to strategies
            for (auto& s : strategies_) s->on_bar(bar, sim_, portfolio_);

            // 4. Process any events generated
            bus_.drain();

            // 5. NAV snapshot
            if (bar.ts_open - last_snap_ts >= cfg_.snapshot_freq * NS_PER_DAY) {
                auto nav = portfolio_.snapshot(bar.ts_open);
                last_snap_ts = bar.ts_open;

                // Risk monitoring
                auto risk_report = risk_.compute(portfolio_, cfg_.bars_per_year);
                if (risk_report.any_breach) {
                    sim_.cancel_all();
                    bus_.emit(RiskEvent{bar.ts_open,
                        RiskEvent::Kind::Halt,
                        risk_report.breach_reason});
                    if (cfg_.verbose)
                        std::cerr << "[RISK] " << risk_report.breach_reason
                                  << " at " << Clock::to_string(bar.ts_open) << "\n";
                }
            }

            ++bar_count;
            if (cfg_.verbose && bar_count % 5000 == 0)
                std::cout << "\r[Backtester] " << bar_count << " / "
                          << timeline.size() << " bars" << std::flush;
        }

        if (cfg_.verbose) std::cout << "\n[Backtester] Done. " << bar_count << " bars\n";

        // Close all positions
        for (auto& s : strategies_) s->on_end(sim_, portfolio_);
        bus_.drain();

        // Final NAV
        if (!feeds_.empty() && !feeds_[0]->bars().empty())
            portfolio_.snapshot(feeds_[0]->bars().back().ts_close);

        // Analytics
        BacktestResult result;
        result.report    = analytics::AnalyticsEngine::compute(
                               portfolio_, cfg_.annual_rf, cfg_.bars_per_year);
        result.nav_curve = portfolio_.nav_curve();
        result.trades    = portfolio_.trades();

        if (cfg_.verbose)
            std::cout << analytics::AnalyticsEngine::format(result.report);

        if (cfg_.run_monte_carlo) {
            result.mc = analytics::AnalyticsEngine::monte_carlo(
                portfolio_, cfg_.mc_simulations, cfg_.bars_per_year,
                cfg_.annual_rf);
            if (cfg_.verbose)
                std::cout << analytics::AnalyticsEngine::format_mc(
                    result.mc, cfg_.mc_simulations);
        }

        return result;
    }

    // ── CSV export ─────────────────────────────────────────────────────────
    void export_nav_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "timestamp,nav,cash,gross_exposure,net_exposure,leverage,daily_pnl\n";
        for (auto& p : portfolio_.nav_curve())
            f << p.ts << "," << p.nav << "," << p.cash << ","
              << p.gross_exposure << "," << p.net_exposure << ","
              << p.leverage << "," << p.daily_pnl << "\n";
    }

    void export_trades_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "instrument,strategy,side,qty,entry,exit,entry_ts,exit_ts,"
             "pnl,commission,slippage\n";
        for (auto& t : portfolio_.trades()) {
            f << ql::ticker(t.instrument) << ","
              << t.strategy_id << ","
              << (t.side==OrderSide::Buy?"LONG":"SHORT") << ","
              << t.qty << "," << t.entry_price << "," << t.exit_price << ","
              << t.entry_ts << "," << t.exit_ts << ","
              << t.pnl << "," << t.commission << "," << t.slippage << "\n";
        }
    }

    const Portfolio&  portfolio() const { return portfolio_; }
    const EventBus&   event_bus() const { return bus_; }

private:
    BacktestConfig                            cfg_;
    Portfolio                                 portfolio_;
    risk::RiskEngine                          risk_;
    EventBus                                  bus_;
    MarketSimulator                           sim_;
    std::vector<std::shared_ptr<DataFeed>>    feeds_;
    std::vector<std::shared_ptr<IStrategy>>   strategies_;
    std::unordered_map<StrategyID, std::shared_ptr<IStrategy>> strategy_map_;
};

} // namespace ql

#include <fstream>
#include <stdexcept>
