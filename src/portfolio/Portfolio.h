#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/portfolio/Portfolio.h
// Institutional portfolio engine.
//
// Tracks: positions, NAV, cash, P&L, margin, buying power, leverage,
//         gross/net exposure, sector exposure, factor exposure (pluggable).
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/SymbolRegistry.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include <optional>
#include <numeric>
#include <algorithm>
#include <map>

namespace ql {

// ── Equity curve point ─────────────────────────────────────────────────────
struct NAVPoint {
    Timestamp ts;
    double    nav;
    double    cash;
    double    gross_exposure;
    double    net_exposure;
    double    leverage;
    double    daily_pnl;
    double    cum_pnl;
    double    drawdown;       // from peak
};

// ── Trade record (for analytics) ──────────────────────────────────────────
struct TradeRecord {
    InstrumentID instrument;
    StrategyID   strategy_id;
    OrderSide    side;
    double       qty;
    double       entry_price;
    double       exit_price;
    Timestamp    entry_ts;
    Timestamp    exit_ts;
    double       pnl;         // realized, net of costs
    double       commission;
    double       slippage;
    double       holding_period_bars;
};

// ── Portfolio ──────────────────────────────────────────────────────────────
class Portfolio {
public:
    explicit Portfolio(double initial_cash,
                       double margin_rate  = 0.25,
                       double max_leverage = 4.0)
        : cash_(initial_cash)
        , initial_cash_(initial_cash)
        , margin_rate_(margin_rate)
        , max_leverage_(max_leverage)
        , peak_nav_(initial_cash) {}

    // ── Fill processing ────────────────────────────────────────────────────
    void on_fill(const Fill& fill) {
        double signed_qty = (fill.side == OrderSide::Buy ||
                             fill.side == OrderSide::BuyToCover)
                            ? fill.quantity : -fill.quantity;

        auto& pos = positions_[fill.instrument];
        pos.instrument  = fill.instrument;
        pos.strategy_id = fill.strategy_id;

        double old_qty  = pos.quantity;
        double new_qty  = old_qty + signed_qty;
        double realized = 0.0;
        double close_qty = 0.0;
        double entry_cost_for_record = pos.avg_cost; // capture before any reset below

        // Compute realized P&L on close/reduce
        if (std::abs(old_qty) > 1e-9 && old_qty * signed_qty < 0) {
            close_qty = std::min(std::abs(old_qty), std::abs(signed_qty));
            realized = close_qty * (fill.price - pos.avg_cost)
                     * (old_qty > 0 ? 1.0 : -1.0);
        }

        // Update average cost
        if (std::abs(new_qty) > 1e-9) {
            if ((old_qty >= 0 && signed_qty > 0) ||
                (old_qty <= 0 && signed_qty < 0)) {
                // Adding to position (same direction)
                pos.avg_cost = (pos.avg_cost * std::abs(old_qty)
                              + fill.price   * std::abs(signed_qty))
                             / std::abs(new_qty);
            } else if (old_qty * new_qty > 0) {
                // Partial close, same sign retained: cost basis stays
            } else {
                // Flip (sign changed) or fresh open from flat: reset basis
                pos.avg_cost = fill.price;
            }
        } else {
            pos.avg_cost = 0;
        }

        pos.quantity          = new_qty;
        pos.realized_pnl     += realized;
        pos.ts_last_fill      = fill.ts;
        if (pos.ts_opened == 0) pos.ts_opened = fill.ts;

        // Cash: debit buy, credit sell
        cash_ -= signed_qty * fill.price + fill.commission;
        realized_pnl_total_ += realized;
        commission_total_   += fill.commission;

        // Record trade on close
        if (std::abs(realized) > 1e-9) {
            TradeRecord tr;
            tr.instrument = fill.instrument;
            tr.strategy_id= fill.strategy_id;
            tr.side       = fill.side;
            tr.qty        = close_qty;
            tr.entry_price= entry_cost_for_record;
            tr.exit_price = fill.price;
            tr.entry_ts   = pos.ts_opened;
            tr.exit_ts    = fill.ts;
            tr.pnl        = realized - fill.commission;
            tr.commission = fill.commission;
            tr.slippage   = fill.slippage;
            trades_.push_back(tr);
        }

        if (new_qty == 0) {
            pos.ts_opened = 0;
        }
    }

    // ── Mark-to-market ─────────────────────────────────────────────────────
    void mark_to_market(InstrumentID id, double price) {
        last_prices_[id] = price;
    }

    // ── NAV snapshot ──────────────────────────────────────────────────────
    NAVPoint snapshot(Timestamp ts) {
        double mkt_val = 0.0, gross = 0.0, long_exp = 0.0, short_exp = 0.0;

        for (auto& [id, pos] : positions_) {
            auto it = last_prices_.find(id);
            if (it == last_prices_.end()) continue;
            double val  = pos.quantity * it->second;
            mkt_val    += val;
            gross      += std::abs(val);
            if (val > 0) long_exp  += val;
            else         short_exp += std::abs(val);
        }

        double nav      = cash_ + mkt_val;
        double leverage = (nav > 0) ? gross / nav : 0;

        // Drawdown
        peak_nav_ = std::max(peak_nav_, nav);
        double dd = (peak_nav_ > 0) ? (peak_nav_ - nav) / peak_nav_ : 0;
        max_drawdown_ = std::max(max_drawdown_, dd);

        double daily_pnl = nav - (nav_curve_.empty() ? initial_cash_
                                                      : nav_curve_.back().nav);

        NAVPoint p{ts, nav, cash_, gross, long_exp - short_exp,
                   leverage, daily_pnl, nav - initial_cash_, dd};
        nav_curve_.push_back(p);
        return p;
    }

    // ── Margin / buying power ──────────────────────────────────────────────
    double current_nav() const {
        double mkt = 0;
        for (auto& [id, pos] : positions_) {
            auto it = last_prices_.find(id);
            if (it != last_prices_.end()) mkt += pos.quantity * it->second;
        }
        return cash_ + mkt;
    }

    double gross_exposure() const {
        double g = 0;
        for (auto& [id, pos] : positions_) {
            auto it = last_prices_.find(id);
            if (it != last_prices_.end()) g += std::abs(pos.quantity * it->second);
        }
        return g;
    }

    double buying_power() const {
        // Simplified Reg-T: cash + margin credit
        double nav = current_nav();
        double used_margin = gross_exposure() * margin_rate_;
        return std::max(0.0, nav - used_margin) / margin_rate_;
    }

    double leverage() const {
        double nav = current_nav();
        return (nav > 0) ? gross_exposure() / nav : 0;
    }

    bool within_leverage(double order_value) const {
        double nav = current_nav();
        return nav > 0 && (gross_exposure() + std::abs(order_value)) / nav <= max_leverage_;
    }

    // ── Sector exposure ────────────────────────────────────────────────────
    std::map<GICSSector, double> sector_exposure() const {
        std::map<GICSSector, double> exp;
        double nav = current_nav();
        for (auto& [id, pos] : positions_) {
            auto it = last_prices_.find(id);
            if (it == last_prices_.end()) continue;
            auto* meta = SymbolRegistry::instance().get_metadata(id);
            GICSSector sec = meta ? meta->sector : GICSSector::Unknown;
            exp[sec] += pos.quantity * it->second / (nav > 0 ? nav : 1);
        }
        return exp;
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    double cash()              const noexcept { return cash_; }
    double initial_cash()      const noexcept { return initial_cash_; }
    double realized_pnl()      const noexcept { return realized_pnl_total_; }
    double commission_total()  const noexcept { return commission_total_; }
    double max_drawdown()      const noexcept { return max_drawdown_; }

    const Position* position(InstrumentID id) const {
        auto it = positions_.find(id);
        return (it != positions_.end()) ? &it->second : nullptr;
    }

    const std::unordered_map<InstrumentID, Position>& all_positions() const {
        return positions_;
    }
    const std::vector<NAVPoint>&     nav_curve() const { return nav_curve_; }
    const std::vector<TradeRecord>&  trades()    const { return trades_; }
    const std::unordered_map<InstrumentID, double>& last_prices() const {
        return last_prices_;
    }

    double last_price(InstrumentID id) const {
        auto it = last_prices_.find(id);
        return (it != last_prices_.end()) ? it->second : 0.0;
    }

private:
    double cash_;
    double initial_cash_;
    double margin_rate_;
    double max_leverage_;
    double peak_nav_;
    double realized_pnl_total_ = 0;
    double commission_total_   = 0;
    double max_drawdown_       = 0;

    std::unordered_map<InstrumentID, Position>    positions_;
    std::unordered_map<InstrumentID, double>      last_prices_;
    std::vector<NAVPoint>                          nav_curve_;
    std::vector<TradeRecord>                       trades_;
};

} // namespace ql
