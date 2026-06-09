#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/execution/SlippageEngine.h
// Pluggable slippage models.
//
// Production systems use empirically-calibrated models. We implement the
// canonical academic/practitioner models as a hierarchy with a plugin
// interface so researchers can inject custom models.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace ql {

// ── Slippage context: everything available at fill time ────────────────────
struct SlippageContext {
    const Bar&    bar;             // current bar
    double        order_qty;       // shares being filled
    OrderSide     side;
    double        mid;             // mid price at order submission
    double        spread;          // bid-ask spread estimate
    double        avg_daily_vol;   // ADV (shares)
    double        realized_vol;    // annualized realized vol
    double        participation;   // target % of volume (0-1)
};

// ── Base interface ─────────────────────────────────────────────────────────
class ISlippageModel {
public:
    virtual ~ISlippageModel() = default;
    // Returns additional cost vs mid (always positive = cost)
    virtual double compute(const SlippageContext& ctx) const = 0;
    virtual std::string name() const = 0;
};

// ── 1. Fixed bps ───────────────────────────────────────────────────────────
class FixedSlippage final : public ISlippageModel {
public:
    explicit FixedSlippage(double bps = 2.0) : bps_(bps) {}
    double compute(const SlippageContext& ctx) const override {
        return ctx.mid * bps_ / 10000.0;
    }
    std::string name() const override { return "Fixed(" + std::to_string(bps_) + "bps)"; }
private:
    double bps_;
};

// ── 2. Spread-based (half-spread + fixed) ─────────────────────────────────
class SpreadSlippage final : public ISlippageModel {
public:
    SpreadSlippage(double spread_frac = 0.5, double fixed_bps = 0.5)
        : spread_frac_(spread_frac), fixed_bps_(fixed_bps) {}
    double compute(const SlippageContext& ctx) const override {
        return ctx.spread * spread_frac_ + ctx.mid * fixed_bps_ / 10000.0;
    }
    std::string name() const override { return "Spread-based"; }
private:
    double spread_frac_;
    double fixed_bps_;
};

// ── 3. Volume participation (linear) ──────────────────────────────────────
class VolumeParticipationSlippage final : public ISlippageModel {
public:
    explicit VolumeParticipationSlippage(double lambda = 0.1)
        : lambda_(lambda) {}
    double compute(const SlippageContext& ctx) const override {
        if (ctx.avg_daily_vol < 1) return 0;
        double pov = ctx.order_qty / ctx.avg_daily_vol;
        return ctx.mid * lambda_ * pov;
    }
    std::string name() const override { return "VolumeParticipation"; }
private:
    double lambda_;
};

// ── 4. Square-root impact (Almgren-Chriss) ────────────────────────────────
// Cost = η * σ * sqrt(Q/V) + ε * σ * (Q/V)
// where η = temporary impact, ε = permanent impact coefficient
class SquareRootImpact final : public ISlippageModel {
public:
    SquareRootImpact(double eta = 0.142, double epsilon = 0.0625)
        : eta_(eta), epsilon_(epsilon) {}

    double compute(const SlippageContext& ctx) const override {
        if (ctx.avg_daily_vol < 1) return 0;
        double q_over_v = ctx.order_qty / ctx.avg_daily_vol;
        double sigma_d  = ctx.realized_vol / std::sqrt(252.0); // daily vol
        // Temporary impact (sqrt model)
        double temp  = eta_     * sigma_d * std::sqrt(q_over_v);
        // Permanent impact (linear model)
        double perm  = epsilon_ * sigma_d * q_over_v;
        return ctx.mid * (temp + perm);
    }

    std::string name() const override { return "SqrtImpact(Almgren-Chriss)"; }

private:
    double eta_;      // temporary impact coefficient
    double epsilon_;  // permanent impact coefficient
};

// ── 5. Temporary + Permanent decomposition (separate terms) ───────────────
class TwoComponentImpact final : public ISlippageModel {
public:
    // Temporary: fades after execution
    // Permanent: shifts mid price permanently (adds to adverse selection)
    TwoComponentImpact(double temp_coeff = 0.1, double perm_coeff = 0.05,
                       double temp_decay = 1.0)
        : temp_(temp_coeff), perm_(perm_coeff), decay_(temp_decay) {}

    double compute(const SlippageContext& ctx) const override {
        if (ctx.avg_daily_vol < 1) return 0;
        double q = ctx.order_qty / ctx.avg_daily_vol;
        double sig = ctx.realized_vol / std::sqrt(252.0);
        double temp_impact = temp_ * sig * std::pow(q, 0.6);
        double perm_impact = perm_ * sig * q;
        return ctx.mid * (temp_impact + perm_impact);
    }

    std::string name() const override { return "TwoComponent"; }

private:
    double temp_, perm_, decay_;
};

// ── 6. Custom plugin ───────────────────────────────────────────────────────
class CustomSlippage final : public ISlippageModel {
public:
    using Fn = std::function<double(const SlippageContext&)>;
    CustomSlippage(std::string name, Fn fn)
        : name_(std::move(name)), fn_(std::move(fn)) {}
    double compute(const SlippageContext& ctx) const override { return fn_(ctx); }
    std::string name() const override { return name_; }
private:
    std::string name_;
    Fn fn_;
};

// ── Composite: sum of multiple models ─────────────────────────────────────
class CompositeSlippage final : public ISlippageModel {
public:
    void add(std::shared_ptr<ISlippageModel> m) { models_.push_back(std::move(m)); }
    double compute(const SlippageContext& ctx) const override {
        double total = 0;
        for (auto& m : models_) total += m->compute(ctx);
        return total;
    }
    std::string name() const override { return "Composite"; }
private:
    std::vector<std::shared_ptr<ISlippageModel>> models_;
};

// ── Commission models ──────────────────────────────────────────────────────
struct CommissionConfig {
    double per_share     = 0.005;     // $/share
    double min_per_order = 1.0;
    double max_pct_value = 0.005;     // 0.5% of order value cap
    double sec_fee_rate  = 0.0000229; // SEC fee on sell side
    bool   ecn_rebate    = false;
    double rebate_per_share = 0.002;

    double calculate(double qty, double price, OrderSide side) const {
        double base = qty * per_share;
        base = std::max(base, min_per_order);
        base = std::min(base, qty * price * max_pct_value);
        if (side == OrderSide::Sell || side == OrderSide::SellShort)
            base += qty * price * sec_fee_rate;
        if (ecn_rebate) base -= qty * rebate_per_share;
        return std::max(base, 0.0);
    }
};

// ── Slippage engine (wires model + commission + spread) ────────────────────
class SlippageEngine {
public:
    SlippageEngine(std::shared_ptr<ISlippageModel> model = nullptr,
                   CommissionConfig comm = {})
        : model_(model ? std::move(model)
                       : std::make_shared<SquareRootImpact>())
        , comm_(comm) {}

    struct FillResult {
        double fill_price;
        double slippage_cost;
        double commission;
        double total_cost;   // slippage + commission
    };

    FillResult compute_fill(const Order& order, const Bar& bar,
                             double adv, double ann_vol) const {
        double mid    = bar.vwap > 0 ? bar.vwap : bar.typical_price();
        double spread = bar.high - bar.low; // approximate; use tick data if available

        SlippageContext ctx{bar, order.quantity, order.side,
                            mid, spread, adv, ann_vol,
                            adv > 0 ? order.quantity / adv : 0};

        double slip = model_->compute(ctx);
        double sign = (order.side == OrderSide::Buy ||
                       order.side == OrderSide::BuyToCover) ? +1.0 : -1.0;

        double fill_px = mid + sign * slip;
        fill_px = std::max(fill_px, 0.01);

        // Clamp to bar range (can't fill outside OHLC in simulation)
        fill_px = std::clamp(fill_px, bar.low, bar.high);

        double comm = comm_.calculate(order.quantity, fill_px, order.side);

        return {fill_px, slip, comm, slip * order.quantity + comm};
    }

    void set_model(std::shared_ptr<ISlippageModel> m) { model_ = std::move(m); }
    const ISlippageModel* model() const { return model_.get(); }

private:
    std::shared_ptr<ISlippageModel> model_;
    CommissionConfig comm_;
};

} // namespace ql
