#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/ranking/StrategyRegistry.h
// Strategy lifecycle registry + degradation monitor.
//
// Lifecycle states:
//   Research -> Validated -> PaperTrading -> Live -> Degraded -> Retired
//
// The registry stores every strategy's lifecycle history, current
// allocation, and rolling performance. The degradation monitor compares
// live rolling metrics against research-phase baselines and raises
// alerts when statistically significant deterioration is detected.
//
// Detected degradation modes:
//   - Alpha decay          (rolling IC trending toward zero)
//   - Sharpe deterioration (rolling Sharpe < threshold vs baseline)
//   - Regime mismatch      (live regime differs from strategy's best regime)
//   - Execution slippage   (realized slippage > backtest assumption)
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/Clock.h"
#include "../regimes/RegimeEngine.h"
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <optional>
#include <cstdio>

namespace ql {
namespace ranking {

// -- Lifecycle states --------------------------------------------------------
enum class LifecycleState : std::uint8_t {
    Research = 0,
    Validated,
    PaperTrading,
    Live,
    Degraded,
    Retired
};

inline const char* lifecycle_name(LifecycleState s) {
    switch(s) {
    case LifecycleState::Research:     return "RESEARCH";
    case LifecycleState::Validated:    return "VALIDATED";
    case LifecycleState::PaperTrading: return "PAPER_TRADING";
    case LifecycleState::Live:         return "LIVE";
    case LifecycleState::Degraded:     return "DEGRADED";
    case LifecycleState::Retired:      return "RETIRED";
    }
    return "?";
}

// -- Lifecycle transition record ---------------------------------------------
struct LifecycleEvent {
    Timestamp      ts;
    LifecycleState from;
    LifecycleState to;
    std::string    reason;
};

// -- Alert ---------------------------------------------------------------------
enum class AlertSeverity : std::uint8_t { Info, Warning, Critical };

struct DegradationAlert {
    Timestamp      ts;
    std::string    strategy_id;
    std::string    type;
    AlertSeverity  severity;
    std::string    message;
    double         metric_value = 0;
    double         baseline_value = 0;
};

// -- Baseline metrics captured at promotion to Live --------------------------
struct PerformanceBaseline {
    double sharpe       = 0;
    double mean_ic      = 0;
    double avg_slippage_bps = 0;
    regimes::Regime     best_regime = regimes::Regime::Unknown;
    double regime_sharpe_in_best = 0;
};

// -- Registered strategy record -----------------------------------------------
struct RegisteredStrategy {
    std::string    id;
    std::string    name;
    LifecycleState state = LifecycleState::Research;
    std::vector<LifecycleEvent> history;

    PerformanceBaseline baseline;

    std::deque<double> rolling_returns;
    std::deque<double> rolling_ic;
    std::deque<double> rolling_slippage_bps;

    double allocation_pct = 0;
    double allocation_usd = 0;

    int    rolling_window = 60;

    void push_return(double ret)        { push(rolling_returns, ret); }
    void push_ic(double ic)             { push(rolling_ic, ic); }
    void push_slippage(double bps)      { push(rolling_slippage_bps, bps); }

    double rolling_sharpe(int bpy=252) const {
        if (rolling_returns.size() < 10) return 0;
        double m=0; for(double x:rolling_returns) m+=x; m/=rolling_returns.size();
        double v=0; for(double x:rolling_returns) v+=(x-m)*(x-m); v/=(rolling_returns.size()-1);
        return (v>0)?m/std::sqrt(v)*std::sqrt(bpy):0;
    }

    double rolling_mean_ic() const {
        if (rolling_ic.empty()) return 0;
        double s=0; for(double x:rolling_ic) s+=x; return s/rolling_ic.size();
    }

    double rolling_avg_slippage() const {
        if (rolling_slippage_bps.empty()) return 0;
        double s=0; for(double x:rolling_slippage_bps) s+=x; return s/rolling_slippage_bps.size();
    }

private:
    void push(std::deque<double>& q, double v) {
        q.push_back(v);
        if ((int)q.size() > rolling_window) q.pop_front();
    }
};

// -- Degradation monitor configuration ----------------------------------------
struct DegradationConfig {
    double sharpe_deterioration_pct = 0.50;
    double ic_decay_threshold       = 0.30;
    double slippage_excess_pct      = 1.50;
    double regime_mismatch_thresh   = -0.5;
    int    min_obs_for_alert        = 20;
};

// -- Strategy registry ----------------------------------------------------------
class StrategyRegistry {
public:
    explicit StrategyRegistry(DegradationConfig cfg = DegradationConfig())
        : cfg_(cfg) {}

    void register_strategy(const std::string& id, const std::string& name) {
        if (strategies_.count(id)) return;
        RegisteredStrategy s;
        s.id = id; s.name = name;
        s.history.push_back({Clock::instance().now(),
                             LifecycleState::Research, LifecycleState::Research,
                             "registered"});
        strategies_[id] = std::move(s);
    }

    bool transition(const std::string& id, LifecycleState to,
                    const std::string& reason = "") {
        auto it = strategies_.find(id);
        if (it == strategies_.end()) return false;
        auto& s = it->second;
        if (!valid_transition(s.state, to)) return false;
        s.history.push_back({Clock::instance().now(), s.state, to, reason});
        s.state = to;
        return true;
    }

    void set_baseline(const std::string& id, PerformanceBaseline baseline) {
        auto it = strategies_.find(id);
        if (it != strategies_.end()) it->second.baseline = baseline;
    }

    void set_allocation(const std::string& id, double pct, double total_capital) {
        auto it = strategies_.find(id);
        if (it == strategies_.end()) return;
        it->second.allocation_pct = pct;
        it->second.allocation_usd = pct * total_capital;
    }

    std::vector<DegradationAlert> observe(
        const std::string& id,
        double bar_return,
        std::optional<double> factor_ic,
        std::optional<double> realized_slippage_bps,
        const regimes::RegimeState* current_regime = nullptr)
    {
        std::vector<DegradationAlert> alerts;
        auto it = strategies_.find(id);
        if (it == strategies_.end()) return alerts;
        auto& s = it->second;

        s.push_return(bar_return);
        if (factor_ic)             s.push_ic(*factor_ic);
        if (realized_slippage_bps) s.push_slippage(*realized_slippage_bps);

        if ((int)s.rolling_returns.size() < cfg_.min_obs_for_alert) return alerts;

        Timestamp ts = Clock::instance().now();

        double rsharpe = s.rolling_sharpe();
        if (s.baseline.sharpe > 0 &&
            rsharpe < s.baseline.sharpe * cfg_.sharpe_deterioration_pct) {
            DegradationAlert a;
            a.ts = ts; a.strategy_id = id; a.type = "sharpe_deterioration";
            a.severity = (rsharpe < 0) ? AlertSeverity::Critical : AlertSeverity::Warning;
            a.metric_value = rsharpe; a.baseline_value = s.baseline.sharpe;
            a.message = "Rolling Sharpe " + fmt(rsharpe) + " vs baseline "
                       + fmt(s.baseline.sharpe);
            alerts.push_back(a);
        }

        if (!s.rolling_ic.empty() && s.baseline.mean_ic != 0) {
            double ric = s.rolling_mean_ic();
            if (std::abs(ric) < std::abs(s.baseline.mean_ic) * cfg_.ic_decay_threshold) {
                DegradationAlert a;
                a.ts = ts; a.strategy_id = id; a.type = "alpha_decay";
                a.severity = AlertSeverity::Warning;
                a.metric_value = ric; a.baseline_value = s.baseline.mean_ic;
                a.message = "Rolling IC " + fmt(ric) + " decayed from baseline "
                           + fmt(s.baseline.mean_ic);
                alerts.push_back(a);
            }
        }

        if (!s.rolling_slippage_bps.empty() && s.baseline.avg_slippage_bps > 0) {
            double rslip = s.rolling_avg_slippage();
            if (rslip > s.baseline.avg_slippage_bps * cfg_.slippage_excess_pct) {
                DegradationAlert a;
                a.ts = ts; a.strategy_id = id; a.type = "execution_deterioration";
                a.severity = AlertSeverity::Warning;
                a.metric_value = rslip; a.baseline_value = s.baseline.avg_slippage_bps;
                a.message = "Realized slippage " + fmt(rslip) + "bps vs baseline "
                           + fmt(s.baseline.avg_slippage_bps) + "bps";
                alerts.push_back(a);
            }
        }

        if (current_regime && s.baseline.best_regime != regimes::Regime::Unknown) {
            if (current_regime->dominant != s.baseline.best_regime &&
                rsharpe < cfg_.regime_mismatch_thresh) {
                DegradationAlert a;
                a.ts = ts; a.strategy_id = id; a.type = "regime_mismatch";
                a.severity = AlertSeverity::Info;
                a.metric_value = rsharpe;
                a.message = std::string("Current regime ")
                          + regimes::regime_name(current_regime->dominant)
                          + " differs from strategy's best regime "
                          + regimes::regime_name(s.baseline.best_regime)
                          + "; rolling Sharpe " + fmt(rsharpe);
                alerts.push_back(a);
            }
        }

        for (auto& a : alerts) {
            if (a.severity == AlertSeverity::Critical && s.state == LifecycleState::Live) {
                transition(id, LifecycleState::Degraded,
                          "auto: " + a.type + " - " + a.message);
            }
            all_alerts_.push_back(a);
        }

        return alerts;
    }

    std::vector<std::string> recommend_deallocation() const {
        std::vector<std::string> ids;
        for (auto& [id, s] : strategies_)
            if (s.state == LifecycleState::Degraded && s.allocation_pct > 0)
                ids.push_back(id);
        return ids;
    }

    std::vector<std::string> recommend_retirement(int min_degraded_bars = 60) const {
        std::vector<std::string> ids;
        for (auto& [id, s] : strategies_) {
            if (s.state != LifecycleState::Degraded) continue;
            Timestamp degraded_since = 0;
            for (auto& ev : s.history)
                if (ev.to == LifecycleState::Degraded) degraded_since = ev.ts;
            if (degraded_since == 0) continue;
            if ((int)s.rolling_returns.size() >= min_degraded_bars &&
                s.rolling_sharpe() < 0)
                ids.push_back(id);
        }
        return ids;
    }

    const RegisteredStrategy* get(const std::string& id) const {
        auto it = strategies_.find(id);
        return (it != strategies_.end()) ? &it->second : nullptr;
    }

    const std::unordered_map<std::string, RegisteredStrategy>& all() const {
        return strategies_;
    }

    const std::vector<DegradationAlert>& alert_log() const { return all_alerts_; }

    void print_status() const {
        std::printf("\n+====================================================================+\n");
        std::printf("|                  STRATEGY LIFECYCLE STATUS                        |\n");
        std::printf("+====================================================================+\n");
        std::printf("  %-20s  %-14s  %8s  %8s  %8s\n",
                    "Strategy","State","Alloc%","RollSh","BaseSh");
        std::printf("  %s\n", std::string(64,'-').c_str());
        for (auto& [id,s] : strategies_) {
            std::printf("  %-20s  %-14s  %7.1f%%  %8.3f  %8.3f\n",
                s.name.c_str(), lifecycle_name(s.state),
                s.allocation_pct*100, s.rolling_sharpe(), s.baseline.sharpe);
        }

        if (!all_alerts_.empty()) {
            std::printf("\n  Recent Alerts:\n");
            int n = std::min(10,(int)all_alerts_.size());
            for (int i=(int)all_alerts_.size()-n;i<(int)all_alerts_.size();++i) {
                auto& a = all_alerts_[i];
                const char* sev = a.severity==AlertSeverity::Critical?"CRIT"
                                 : a.severity==AlertSeverity::Warning?"WARN":"INFO";
                std::printf("    [%s] %-20s  %-22s  %s\n",
                    sev, a.strategy_id.c_str(), a.type.c_str(), a.message.c_str());
            }
        }
    }

private:
    static bool valid_transition(LifecycleState from, LifecycleState to) {
        using S = LifecycleState;
        switch(from) {
        case S::Research:     return to==S::Validated || to==S::Retired;
        case S::Validated:    return to==S::PaperTrading || to==S::Retired;
        case S::PaperTrading: return to==S::Live || to==S::Research || to==S::Retired;
        case S::Live:         return to==S::Degraded || to==S::Retired;
        case S::Degraded:     return to==S::Live || to==S::Retired;
        case S::Retired:      return false;
        }
        return false;
    }

    static std::string fmt(double v) {
        char b[32]; std::snprintf(b,sizeof(b),"%.3f",v); return b;
    }

    DegradationConfig cfg_;
    std::unordered_map<std::string, RegisteredStrategy> strategies_;
    std::vector<DegradationAlert> all_alerts_;
};

} // namespace ranking
} // namespace ql
