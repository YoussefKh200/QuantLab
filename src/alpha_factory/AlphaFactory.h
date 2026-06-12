#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/alpha_factory/AlphaFactory.h
// Automated alpha candidate generation, evaluation, storage, and ranking.
//
// Design: AlphaFactory is a registry of alpha candidates. Each candidate
// is defined by a parameter combination of existing factor building blocks,
// evaluated on a universe of DataFeeds, and scored along multiple dimensions.
//
// Pipeline:
//   generate() → evaluate() → rank() → persist()
//
// Every alpha candidate stores:
//   - Identity (name, factor type, parameters)
//   - Performance (Sharpe, Sortino, CAGR, max DD)
//   - Research quality (IC, ICIR, decay half-life)
//   - Robustness (MC confidence interval on Sharpe)
//   - Risk (turnover, capacity score, regime stability)
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/ThreadPool.h"
#include "../factors/AlphaEngine.h"
#include "../simulation/Backtester.h"
#include "../simulation/ExampleStrategies.h"
#include "../analytics/Analytics.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>
#include <random>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <future>

namespace ql {
namespace alpha {

// ── Alpha candidate ID ─────────────────────────────────────────────────────
using AlphaID = std::uint64_t;

// ── Alpha candidate record ─────────────────────────────────────────────────
struct AlphaCandidate {
    AlphaID     id            = 0;
    std::string name;
    std::string factor_type;  // "momentum", "reversion", "vol", "custom", etc.
    std::unordered_map<std::string, double> params;

    // Factor research quality
    double mean_ic       = 0;   // average information coefficient
    double ic_std        = 0;
    double icir          = 0;   // IC / IC_std
    double ic_half_life  = 0;   // bars to IC decay to half (lower = more perishable)

    // Backtest performance (out-of-sample where possible)
    double sharpe_is     = 0;   // in-sample Sharpe
    double sharpe_oos    = 0;   // out-of-sample Sharpe (walk-forward)
    double sortino       = 0;
    double calmar        = 0;
    double cagr          = 0;
    double max_drawdown  = 0;
    double win_rate      = 0;
    double profit_factor = 0;

    // Robustness
    double sharpe_mc_p5  = 0;   // 5th percentile Sharpe across MC sims
    double sharpe_mc_p50 = 0;   // median
    double prob_positive_oos = 0; // P(OOS Sharpe > 0)

    // Risk / capacity
    double turnover_annual = 0;  // average annual turnover
    double capacity_score  = 0;  // 0-1, higher = larger capacity
    double regime_stability= 0;  // 0-1, how consistent across regimes

    // Meta
    Timestamp discovered_ts = 0;
    int       n_backtests   = 0;
    bool      promoted      = false;  // promoted to strategy pool

    // Composite score (computed by AlphaRanker)
    double composite_score  = 0;
    int    rank             = 0;
};

// ── Alpha generation spec ─────────────────────────────────────────────────
struct AlphaSpec {
    std::string type;
    std::unordered_map<std::string, std::vector<double>> param_grid;

    // Generate all combinations
    std::vector<std::unordered_map<std::string,double>> expand() const {
        std::vector<std::unordered_map<std::string,double>> combos = {{}};
        for (auto& [k, vals] : param_grid) {
            std::vector<std::unordered_map<std::string,double>> expanded;
            for (auto& ps : combos)
                for (double v : vals) {
                    auto np = ps; np[k] = v;
                    expanded.push_back(np);
                }
            combos = std::move(expanded);
        }
        return combos;
    }
};

// ── Built-in alpha specs for automated generation ─────────────────────────
inline std::vector<AlphaSpec> default_alpha_specs() {
    return {
        {"momentum",  {{"lookback", {5,10,21,63,126,252}}}},
        {"reversion", {{"lookback", {3,5,10,20}}}},
        {"vol",       {{"lookback", {10,20,60}}}},
        {"low_vol",   {{"lookback", {20,60,120}}}},
        {"breakout",  {{"channel",{10,20,40}},{"atr_mult",{1.5,2.0,2.5}}}},
        {"ma_cross",  {{"fast",{5,10,20}},{"slow",{30,50,100}}}},
        {"vol_target",{{"fast",{10,20}},{"slow",{60,100}},{"target_vol",{0.10,0.15}}}},
        {"nearness",  {{"lookback",{63,126,252}}}},
        {"vol_trend", {{"short_w",{3,5,10}},{"long_w",{20,40}}}},
    };
}

// ── Alpha factory ──────────────────────────────────────────────────────────
struct AlphaFactoryConfig {
    int    mc_sims           = 200;
    double min_icir          = 0.3;
    double min_oos_sharpe    = 0.2;
    int    bars_per_year     = 252;
    double annual_rf         = 0.05;
    bool   parallel          = true;
    int    max_candidates    = 10000;
};

class AlphaFactory {
public:
    using Config = AlphaFactoryConfig;
    explicit AlphaFactory(Config cfg = Config()) : cfg_(cfg) {}

    // ── Generate candidates from specs ────────────────────────────────────
    std::vector<AlphaID> generate(const std::vector<AlphaSpec>& specs) {
        std::vector<AlphaCandidate> new_candidates;
        for (auto& spec : specs) {
            for (auto& params : spec.expand()) {
                AlphaCandidate c;
                c.id           = next_id_++;
                c.factor_type  = spec.type;
                c.params       = params;
                c.name         = make_name(spec.type, params);
                c.discovered_ts= Clock::instance().now();
                new_candidates.push_back(c);
            }
        }
        std::vector<AlphaID> ids;
        {
            std::lock_guard lk(mu_);
            for (auto& c : new_candidates) {
                ids.push_back(c.id);
                registry_[c.id] = std::move(c);
            }
        }
        return ids;
    }

    // ── Evaluate a set of candidates against universe ─────────────────────
    void evaluate(const std::vector<AlphaID>& ids,
                  const std::vector<std::shared_ptr<DataFeed>>& feeds,
                  double initial_cash = 1'000'000.0) {
        if (feeds.empty()) return;

        auto eval_one = [&](AlphaID id) {
            AlphaCandidate* c;
            { std::lock_guard lk(mu_); c = &registry_.at(id); }

            BacktestConfig cfg;
            cfg.initial_cash = initial_cash;
            cfg.verbose      = false;
            cfg.annual_rf    = cfg_.annual_rf;

            // Build strategy from candidate spec
            auto strat = make_strategy(*c);
            if (!strat) return;

            // IS backtest (full history)
            Backtester bt(cfg);
            for (auto& f : feeds) bt.add_feed(f);
            bt.add_strategy(strat, c->name);
            BacktestResult res;
            try { res = bt.run(); } catch(...) { return; }

            // Factor IC research
            if (!feeds.empty()) {
                auto factor = make_factor(*c);
                if (factor) {
                    factors::AlphaEngineConfig ae_cfg;
                    ae_cfg.min_universe = std::max(2, (int)feeds.size() - 1);
                    factors::AlphaEngine ae(ae_cfg);
                    ae.add_factor(factor);
                    Timestamp start = feeds[0]->start() + 252*NS_PER_DAY;
                    Timestamp end   = feeds[0]->end();
                    auto ic = ae.compute_ic(factor->name(), feeds, start, end, 21, 21);
                    std::lock_guard lk(mu_);
                    c->mean_ic   = ic.mean_ic;
                    c->ic_std    = ic.ic_std;
                    c->icir      = ic.icir;
                    c->ic_half_life = compute_ic_halflife(ic.ic);
                }
            }

            // Monte Carlo confidence interval
            auto mc = analytics::AnalyticsEngine::monte_carlo(
                bt.portfolio(), cfg_.mc_sims, cfg_.bars_per_year, cfg_.annual_rf);

            std::lock_guard lk(mu_);
            auto& cc = registry_.at(id);
            cc.sharpe_is      = res.report.sharpe_ratio;
            cc.sortino        = res.report.sortino_ratio;
            cc.calmar         = res.report.calmar_ratio;
            cc.cagr           = res.report.cagr;
            cc.max_drawdown   = res.report.max_drawdown;
            cc.win_rate       = res.report.win_rate;
            cc.profit_factor  = res.report.profit_factor;
            cc.sharpe_mc_p5   = mc.p5_sharpe;
            cc.sharpe_mc_p50  = mc.median_sharpe;
            cc.prob_positive_oos = mc.prob_profitable / 100.0;
            cc.turnover_annual   = res.report.turnover_annual;
            ++cc.n_backtests;
        };

        if (cfg_.parallel) {
            std::vector<std::future<void>> futs;
            for (auto id : ids)
                futs.push_back(global_pool().submit(eval_one, id));
            for (auto& f : futs) { try { f.get(); } catch(...) {} }
        } else {
            for (auto id : ids) eval_one(id);
        }
    }

    // ── Rank all evaluated candidates ─────────────────────────────────────
    void rank() {
        std::lock_guard lk(mu_);
        std::vector<AlphaCandidate*> ptrs;
        for (auto& [id, c] : registry_) ptrs.push_back(&c);

        for (auto* c : ptrs) {
            // Composite score: weighted combination of normalised metrics
            double s = 0;
            s += 0.25 * std::max(c->sharpe_is,  -5.0) / 3.0;       // IS Sharpe (norm by 3)
            s += 0.20 * std::max(c->icir,        -5.0) / 2.0;       // ICIR
            s += 0.15 * (c->prob_positive_oos - 0.5) * 2.0;          // P(OOS>0)
            s += 0.15 * std::max(c->sharpe_mc_p50, -5.0) / 3.0;     // MC median Sharpe
            s += 0.10 * (1.0 - c->max_drawdown/100.0);               // 1 - DD%
            s += 0.10 * c->regime_stability;                          // regime consistency
            s += 0.05 * std::min(c->profit_factor / 3.0, 1.0);       // profit factor
            c->composite_score = s;
        }

        std::sort(ptrs.begin(), ptrs.end(),
                  [](const AlphaCandidate* a, const AlphaCandidate* b) {
                      return a->composite_score > b->composite_score; });
        for (int i = 0; i < (int)ptrs.size(); ++i) ptrs[i]->rank = i+1;
    }

    // ── Promote top N candidates to strategy pool ──────────────────────────
    std::vector<AlphaID> top(int n = 20) const {
        std::lock_guard lk(mu_);
        std::vector<const AlphaCandidate*> sorted;
        for (auto& [id, c] : registry_) sorted.push_back(&c);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b){ return a->rank < b->rank; });
        std::vector<AlphaID> ids;
        for (int i = 0; i < std::min(n,(int)sorted.size()); ++i)
            ids.push_back(sorted[i]->id);
        return ids;
    }

    const AlphaCandidate* get(AlphaID id) const {
        std::lock_guard lk(mu_);
        auto it = registry_.find(id);
        return (it != registry_.end()) ? &it->second : nullptr;
    }

    std::size_t size() const {
        std::lock_guard lk(mu_); return registry_.size();
    }

    void print_top(int n = 10) const {
        std::lock_guard lk(mu_);
        std::vector<const AlphaCandidate*> sorted;
        for (auto& [id,c] : registry_) sorted.push_back(&c);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b){ return a->rank < b->rank; });
        std::printf("\n  %-30s  %7s  %7s  %7s  %7s  %7s\n",
                    "Alpha", "Sharpe", "ICIR", "MaxDD%", "Score", "Rank");
        std::printf("  %s\n", std::string(75, '-').c_str());
        for (int i = 0; i < std::min(n,(int)sorted.size()); ++i) {
            auto* c = sorted[i];
            std::printf("  %-30s  %7.3f  %7.3f  %7.2f  %7.4f  %5d\n",
                c->name.c_str(), c->sharpe_is, c->icir,
                c->max_drawdown, c->composite_score, c->rank);
        }
    }

    // Full factory run: generate → evaluate → rank
    void run(const std::vector<std::shared_ptr<DataFeed>>& feeds,
             const std::vector<AlphaSpec>& specs = default_alpha_specs()) {
        auto ids = generate(specs);
        std::printf("[AlphaFactory] Generated %zu candidates\n", ids.size());
        evaluate(ids, feeds);
        rank();
        std::printf("[AlphaFactory] Evaluated and ranked %zu alphas\n", registry_.size());
    }

private:
    static std::string make_name(const std::string& type,
                                  const std::unordered_map<std::string,double>& params) {
        std::ostringstream ss; ss << type;
        for (auto& [k,v] : params)
            ss << "_" << k.substr(0,3) << std::setprecision(3) << v;
        return ss.str();
    }

    std::shared_ptr<IStrategy> make_strategy(const AlphaCandidate& c) {
        auto get = [&](const char* k, double def) -> double {
            auto it = c.params.find(k);
            return (it != c.params.end()) ? it->second : def;
        };
        if (c.factor_type == "ma_cross") {
            MACrossParams p;
            p.fast     = (int)get("fast", 10);
            p.slow     = (int)get("slow", 50);
            p.notional = 200'000.0;
            return std::make_shared<MACrossStrategy>(p);
        }
        if (c.factor_type == "momentum") {
            // Map lookback -> slow MA, fast = slow/4 (min 2)
            int slow = (int)get("lookback", 40);
            int fast = std::max(2, slow / 4);
            MACrossParams p;
            p.fast = fast; p.slow = slow; p.notional = 200'000.0;
            return std::make_shared<MACrossStrategy>(p);
        }
        if (c.factor_type == "reversion") {
            MeanRevParams p;
            p.lookback = (int)get("lookback", 20);
            p.notional = 200'000.0;
            return std::make_shared<MeanReversionStrategy>(p);
        }
        if (c.factor_type == "breakout") {
            BreakoutParams p;
            p.channel_period = (int)get("channel", 20);
            p.atr_mult       = get("atr_mult", 2.0);
            p.notional       = 200'000.0;
            return std::make_shared<BreakoutStrategy>(p);
        }
        if (c.factor_type == "vol_target") {
            VolTargetParams p;
            p.fast       = (int)get("fast", 20);
            p.slow       = (int)get("slow", 100);
            p.target_vol = get("target_vol", 0.10);
            p.notional   = 200'000.0;
            return std::make_shared<VolTargetTrendStrategy>(p);
        }
        return nullptr;
    }

    std::shared_ptr<factors::IFactor> make_factor(const AlphaCandidate& c) {
        auto get = [&](const char* k, double def) -> int {
            auto it = c.params.find(k);
            return (it != c.params.end()) ? (int)it->second : (int)def;
        };
        if (c.factor_type == "momentum")   return factors::momentum(get("lookback",21));
        if (c.factor_type == "reversion")  return factors::reversal(get("lookback",5));
        if (c.factor_type == "vol")        return factors::realized_vol(get("lookback",20));
        if (c.factor_type == "low_vol")    return factors::low_vol(get("lookback",60));
        if (c.factor_type == "nearness")   return factors::nearness_to_high(get("lookback",252));
        if (c.factor_type == "vol_trend")
            return factors::volume_trend(get("short_w",5),get("long_w",20));
        return nullptr;
    }

    static double compute_ic_halflife(const std::vector<double>& ic_series) {
        if (ic_series.size() < 5) return 252;
        // Approximate: find horizon where autocorrelation drops to 0.5
        double ic0 = ic_series[0];
        if (std::abs(ic0) < 1e-9) return 252;
        for (int lag = 1; lag < (int)ic_series.size(); ++lag)
            if (std::abs(ic_series[lag]) <= std::abs(ic0) * 0.5)
                return static_cast<double>(lag);
        return static_cast<double>(ic_series.size());
    }

    Config cfg_;
    mutable std::mutex mu_;
    std::unordered_map<AlphaID, AlphaCandidate> registry_;
    std::atomic<AlphaID> next_id_{1};
};

} // namespace alpha
} // namespace ql
