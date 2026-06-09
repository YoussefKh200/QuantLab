#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/risk/RiskEngine.h
// Institutional risk management.
//
// Pre-trade: position limits, leverage checks, concentration
// Intra-day: drawdown halt, daily loss limit, vol target
// Post-trade: VaR, ES, stress testing, scenario analysis
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../portfolio/Portfolio.h"
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <random>
#include <functional>

namespace ql {
namespace risk {

// ── Risk limits configuration ──────────────────────────────────────────────
struct RiskLimits {
    // Portfolio-level
    double max_gross_leverage     = 4.0;
    double max_net_leverage       = 2.0;
    double max_drawdown_pct       = 0.20;   // halt at 20% DD
    double daily_loss_limit_pct   = 0.03;   // 3% daily loss limit
    double max_var_pct            = 0.05;   // 5% of NAV at 99% confidence

    // Position-level
    double max_position_pct       = 0.20;   // 20% of NAV per position
    double max_sector_pct         = 0.40;   // 40% of NAV per sector
    double max_single_name_short  = 0.10;   // 10% short any single name

    // Volatility target
    bool   vol_targeting_enabled  = false;
    double target_annual_vol      = 0.10;   // 10% target portfolio vol
    int    vol_lookback_days      = 60;

    // Concentration
    double min_positions          = 5;      // minimum diversification
};

// ── Risk decision ──────────────────────────────────────────────────────────
enum class RiskDecision { Allow, Block, Reduce, Warn };

struct RiskCheck {
    RiskDecision decision;
    std::string  reason;
    double       allowed_qty = 0;   // if Reduce
};

// ── VaR engine ────────────────────────────────────────────────────────────
class VaREngine {
public:
    // Historical simulation VaR
    static double historical_var(const std::vector<double>& returns,
                                  double confidence = 0.99,
                                  double portfolio_value = 1.0) {
        if (returns.size() < 30) return 0;
        auto sorted = returns;
        std::sort(sorted.begin(), sorted.end());
        int idx = static_cast<int>((1.0 - confidence) * sorted.size());
        return -sorted[std::max(0, idx)] * portfolio_value;
    }

    // Parametric VaR (normal assumption)
    static double inv_normal(double p) {
        if (p < 0.5) return -inv_normal(1-p);
        double t = std::sqrt(-2*std::log(1-p));
        double c[] = {2.515517, 0.802853, 0.010328};
        double d[] = {1.432788, 0.189269, 0.001308};
        return t - (c[0]+c[1]*t+c[2]*t*t)/(1+d[0]*t+d[1]*t*t+d[2]*t*t*t);
    }
    static double parametric_var(double daily_vol, double confidence = 0.99,
                                  double portfolio_value = 1.0, int horizon = 1) {
        double z = inv_normal(confidence);
        return z * daily_vol * std::sqrt(horizon) * portfolio_value;
    }

    // Expected Shortfall (CVaR) — average loss beyond VaR
    static double expected_shortfall(const std::vector<double>& returns,
                                      double confidence = 0.99,
                                      double portfolio_value = 1.0) {
        if (returns.size() < 30) return 0;
        auto sorted = returns;
        std::sort(sorted.begin(), sorted.end());
        int cutoff = static_cast<int>((1.0 - confidence) * sorted.size());
        if (cutoff == 0) return 0;
        double sum = 0;
        for (int i = 0; i < cutoff; ++i) sum += sorted[i];
        return -sum / cutoff * portfolio_value;
    }

    // Monte Carlo VaR
    static double mc_var(double daily_vol, double confidence = 0.99,
                          double portfolio_value = 1.0, int horizon = 1,
                          int simulations = 10000, unsigned seed = 42) {
        std::mt19937_64 rng(seed);
        std::normal_distribution<double> N(0, daily_vol);
        std::vector<double> path_rets(simulations);
        for (auto& r : path_rets) {
            double cum = 0;
            for (int d = 0; d < horizon; ++d) cum += N(rng);
            path_rets[&r - &path_rets[0]] = cum;
        }
        std::sort(path_rets.begin(), path_rets.end());
        int idx = static_cast<int>((1.0-confidence)*simulations);
        return -path_rets[idx] * portfolio_value;
    }
};

// ── Stress test scenarios ─────────────────────────────────────────────────
struct StressScenario {
    std::string name;
    std::unordered_map<InstrumentID, double> price_shocks;  // % shocks
    double                                    rate_shock = 0;
    double                                    vol_shock  = 0;
};

inline StressScenario dotcom_crash() {
    // Tech -80%, broad market -50% (2000-2002 approximation)
    return {"Dotcom Crash", {}, 0.0, 0.5};
}

inline StressScenario gfc_2008() {
    return {"GFC 2008", {}, 0.02, 0.6};  // rates up, vol up 60%
}

inline StressScenario covid_crash() {
    return {"COVID March 2020", {}, -0.005, 1.0};  // vol doubles
}

// ── Kelly criterion sizing ────────────────────────────────────────────────
inline double kelly_fraction(double win_rate, double avg_win, double avg_loss,
                              double fraction = 0.5) {
    if (avg_loss <= 0 || win_rate <= 0 || win_rate >= 1) return 0;
    double b    = avg_win / avg_loss;   // win/loss ratio
    double f    = (win_rate * b - (1-win_rate)) / b;  // full Kelly
    return std::max(0.0, std::min(fraction * f, 1.0)); // fractional Kelly
}

// ── Volatility targeting ───────────────────────────────────────────────────
inline double vol_target_scalar(double realized_vol, double target_vol) {
    if (realized_vol < 1e-6) return 1.0;
    return std::min(target_vol / realized_vol, 2.0);  // cap at 2x
}

// ── Risk engine ───────────────────────────────────────────────────────────
class RiskEngine {
public:
    explicit RiskEngine(RiskLimits limits = {}) : limits_(limits) {}

    // ── Pre-trade check ────────────────────────────────────────────────────
    RiskCheck check_order(const Order& order, const Portfolio& pf,
                           double last_price) const {
        double nav = pf.current_nav();
        if (nav <= 0) return {RiskDecision::Block, "Zero NAV"};

        // Max drawdown halt
        if (pf.max_drawdown() >= limits_.max_drawdown_pct)
            return {RiskDecision::Block,
                    "Max drawdown " + pct(pf.max_drawdown()) + " breached"};

        // Daily loss halt
        if (!pf.nav_curve().empty()) {
            const auto& today = pf.nav_curve().back();
            double daily_dd = today.daily_pnl / nav;
            if (daily_dd <= -limits_.daily_loss_limit_pct)
                return {RiskDecision::Block,
                        "Daily loss limit " + pct(-daily_dd) + " breached"};
        }

        double order_value = order.quantity * last_price;

        // Position concentration
        const auto* pos = pf.position(order.instrument);
        double pos_value = (pos ? std::abs(pos->quantity) * last_price : 0)
                         + std::abs(order_value);
        if (pos_value / nav > limits_.max_position_pct) {
            double allowed_value = nav * limits_.max_position_pct
                                 - (pos ? std::abs(pos->quantity) * last_price : 0);
            double allowed_qty = std::max(0.0, allowed_value / last_price);
            if (allowed_qty < 1)
                return {RiskDecision::Block, "Position limit breached"};
            return {RiskDecision::Reduce, "Position limit: reduced", allowed_qty};
        }

        // Gross leverage
        if (!pf.within_leverage(order_value))
            return {RiskDecision::Block,
                    "Gross leverage " + std::to_string(pf.leverage()) + "x breached"};

        return {RiskDecision::Allow, ""};
    }

    // ── Portfolio-level risk metrics ───────────────────────────────────────
    struct PortfolioRisk {
        double var_99_1d    = 0;
        double es_99_1d     = 0;
        double var_95_10d   = 0;
        double realized_vol = 0;    // annualized
        double max_drawdown = 0;
        double leverage     = 0;
        bool   any_breach   = false;
        std::string breach_reason;
    };

    PortfolioRisk compute(const Portfolio& pf, int bars_per_year = 252) const {
        PortfolioRisk r;
        const auto& curve = pf.nav_curve();
        if (curve.size() < 20) return r;

        // Extract daily returns
        std::vector<double> rets;
        rets.reserve(curve.size()-1);
        for (std::size_t i = 1; i < curve.size(); ++i) {
            double prev = curve[i-1].nav;
            if (prev > 0) rets.push_back((curve[i].nav - prev) / prev);
        }

        double nav = pf.current_nav();
        r.var_99_1d    = VaREngine::historical_var(rets, 0.99, nav);
        r.es_99_1d     = VaREngine::expected_shortfall(rets, 0.99, nav);
        r.var_95_10d   = VaREngine::parametric_var(
                             portfolio_vol(rets) / std::sqrt(bars_per_year),
                             0.95, nav, 10);
        r.realized_vol = portfolio_vol(rets);
        r.max_drawdown = pf.max_drawdown();
        r.leverage     = pf.leverage();

        // Breach checks
        if (pf.max_drawdown() >= limits_.max_drawdown_pct) {
            r.any_breach = true;
            r.breach_reason = "Max drawdown";
        } else if (pf.leverage() > limits_.max_gross_leverage) {
            r.any_breach = true;
            r.breach_reason = "Gross leverage";
        }

        return r;
    }

    // ── Stress test ────────────────────────────────────────────────────────
    double stress_test(const Portfolio& pf, const StressScenario& scenario) const {
        double pnl = 0;
        for (auto& [id, pos] : pf.all_positions()) {
            double px = pf.last_price(id);
            double shock = 0;
            auto it = scenario.price_shocks.find(id);
            if (it != scenario.price_shocks.end()) shock = it->second;
            else shock = -0.30; // default broad market shock
            pnl += pos.quantity * px * shock;
        }
        return pnl;
    }

    // ── Volatility target scaling ──────────────────────────────────────────
    double vol_scale(const Portfolio& pf) const {
        if (!limits_.vol_targeting_enabled) return 1.0;
        const auto& curve = pf.nav_curve();
        if ((int)curve.size() < limits_.vol_lookback_days + 1) return 1.0;
        std::vector<double> rets;
        int n = std::min((int)curve.size()-1, limits_.vol_lookback_days);
        int start = (int)curve.size()-1-n;
        for (int i = start+1; i < (int)curve.size(); ++i) {
            double prev = curve[i-1].nav;
            if (prev > 0) rets.push_back((curve[i].nav-prev)/prev);
        }
        double rv = portfolio_vol(rets);
        return vol_target_scalar(rv, limits_.target_annual_vol);
    }

    const RiskLimits& limits() const { return limits_; }
    void set_limits(RiskLimits l) { limits_ = std::move(l); }

private:
    static double portfolio_vol(const std::vector<double>& rets, int bars_per_year=252) {
        if (rets.size() < 2) return 0;
        double m = 0; for (double r : rets) m += r; m /= rets.size();
        double var = 0; for (double r : rets) var += (r-m)*(r-m);
        return std::sqrt(var/(rets.size()-1) * bars_per_year);
    }

    static std::string pct(double v) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f%%", v*100); return b;
    }

    RiskLimits limits_;
};

} // namespace risk
} // namespace ql
