#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/MarketSimulator.h
// Simulated exchange matching engine.
//
// Supports: Market, Limit, Stop, StopLimit, IOC, FOK, GTC, MOO, MOC
// Models:   Queue position, partial fills, latency, price improvement
//
// Architecture: per-instrument order book; matching triggered by bars/ticks.
// Deterministic replay guaranteed by simulation clock.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/Clock.h"
#include "../events/EventBus.h"
#include "../execution/SlippageEngine.h"
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <atomic>
#include <optional>
#include <cmath>

namespace ql {

// ── Order book level ──────────────────────────────────────────────────────
struct BookLevel {
    double price;
    double total_size;
    int    order_count;
};

// ── Simulated order book ──────────────────────────────────────────────────
class SimOrderBook {
public:
    // Returns estimated queue position as fraction of level size
    double queue_position_fraction(const Order& order) const {
        // Simplified: assume we're at the back of the queue
        return 0.8;  // 80% of level size ahead of us
    }

    void add_order(const Order& order) {
        if (order.side == OrderSide::Buy || order.side == OrderSide::BuyToCover)
            bids_[order.limit_price].total_size += order.quantity;
        else
            asks_[order.limit_price].total_size += order.quantity;
    }

    void remove_order(const Order& order) {
        if (order.side == OrderSide::Buy || order.side == OrderSide::BuyToCover) {
            auto it = bids_.find(order.limit_price);
            if (it != bids_.end()) it->second.total_size -= order.quantity;
        } else {
            auto it = asks_.find(order.limit_price);
            if (it != asks_.end()) it->second.total_size -= order.quantity;
        }
    }

    double best_bid() const { return bids_.empty() ? 0 : bids_.rbegin()->first; }
    double best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }

private:
    std::map<double, BookLevel> bids_;  // descending (best at back)
    std::map<double, BookLevel> asks_;  // ascending  (best at front)
};

// ── Per-instrument pending order set ─────────────────────────────────────
struct PendingOrders {
    std::vector<Order> orders;

    void add(Order o) { orders.push_back(std::move(o)); }

    bool cancel(OrderID id) {
        for (auto& o : orders) {
            if (o.id == id && o.is_live()) {
                o.status = OrderStatus::Cancelled;
                o.ts_cancelled = Clock::instance().now();
                return true;
            }
        }
        return false;
    }

    void remove_dead() {
        orders.erase(std::remove_if(orders.begin(), orders.end(),
            [](const Order& o){ return !o.is_live(); }), orders.end());
    }
};

// ── Market simulator ──────────────────────────────────────────────────────
class MarketSimulator {
public:
    MarketSimulator(EventBus& bus, SlippageEngine slip = {})
        : bus_(bus), slip_(std::move(slip)) {}

    // ── Order submission ──────────────────────────────────────────────────
    OrderID submit(Order order) {
        order.id         = ++order_counter_;
        order.ts_created = Clock::instance().now();
        order.ts_sent    = Clock::instance().order_ack_time();
        order.status     = OrderStatus::New;

        // Immediate IOC/FOK: will attempt fill on next bar
        pending_[order.instrument].add(order);
        bus_.emit(OrderEvent{order.ts_created, order, OrderEvent::Action::New});
        return order.id;
    }

    bool cancel(OrderID id, InstrumentID instrument) {
        auto& p = pending_[instrument];
        if (p.cancel(id)) {
            // Emit cancellation
            for (auto& o : p.orders) {
                if (o.id == id) {
                    bus_.emit(OrderEvent{Clock::instance().now(), o,
                                        OrderEvent::Action::Cancel});
                    break;
                }
            }
            return true;
        }
        return false;
    }

    void cancel_all(StrategyID strat_id = 0) {
        for (auto& [inst, p] : pending_) {
            for (auto& o : p.orders) {
                if ((strat_id == 0 || o.strategy_id == strat_id) && o.is_live()) {
                    o.status = OrderStatus::Cancelled;
                    o.ts_cancelled = Clock::instance().now();
                    bus_.emit(OrderEvent{o.ts_cancelled, o,
                                        OrderEvent::Action::Cancel});
                }
            }
            p.remove_dead();
        }
    }

    // ── Bar processing: attempt to fill all pending orders ────────────────
    void on_bar(const Bar& bar, double adv = 0, double ann_vol = 0.20) {
        auto& p = pending_[bar.instrument];
        if (p.orders.empty()) return;

        Clock::instance().advance_to(bar.ts_close);

        std::vector<Order*> to_fill;
        for (auto& o : p.orders) {
            if (!o.is_live()) continue;
            if (try_trigger(o, bar)) to_fill.push_back(&o);
        }

        for (auto* op : to_fill) {
            execute_fill(*op, bar, adv, ann_vol);
        }

        // Expire Day orders on session close
        for (auto& o : p.orders) {
            if (o.is_live() && o.tif == TimeInForce::Day) {
                o.status = OrderStatus::Expired;
                bus_.emit(OrderEvent{bar.ts_close, o, OrderEvent::Action::Cancel});
            }
        }

        p.remove_dead();
    }

    // ── Tick processing ────────────────────────────────────────────────────
    void on_tick(const Tick& tick) {
        auto& p = pending_[tick.instrument];
        for (auto& o : p.orders) {
            if (!o.is_live()) continue;
            if (try_trigger_tick(o, tick)) {
                // Synthesize a minimal bar from tick for slippage context
                Bar b;
                b.instrument = tick.instrument;
                b.ts_open = b.ts_close = tick.ts;
                b.open = b.high = b.low = b.close = tick.price;
                b.vwap = tick.microprice();
                b.volume = tick.size;
                execute_fill(o, b, 0, 0.20);
            }
        }
        p.remove_dead();
    }

    std::size_t pending_count(InstrumentID inst = 0) const {
        if (inst != 0) {
            auto it = pending_.find(inst);
            return (it == pending_.end()) ? 0 : it->second.orders.size();
        }
        std::size_t n = 0;
        for (auto& [i, p] : pending_) n += p.orders.size();
        return n;
    }

    void set_slippage(SlippageEngine s) { slip_ = std::move(s); }

private:
    // ── Trigger check ─────────────────────────────────────────────────────
    bool try_trigger(const Order& o, const Bar& bar) const {
        switch (o.type) {
        case OrderType::Market:
        case OrderType::MOO:
        case OrderType::MOC:
            return true;

        case OrderType::Limit:
            if (o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                return bar.low <= o.limit_price;
            return bar.high >= o.limit_price;

        case OrderType::Stop:
            if (o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                return bar.high >= o.stop_price;
            return bar.low <= o.stop_price;

        case OrderType::StopLimit:
            if (o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                return bar.high >= o.stop_price && bar.low <= o.limit_price;
            return bar.low <= o.stop_price && bar.high >= o.limit_price;

        case OrderType::IOC:
        case OrderType::FOK:
            return true;  // attempt immediate fill; cancel residual

        default: return false;
        }
    }

    bool try_trigger_tick(const Order& o, const Tick& t) const {
        switch (o.type) {
        case OrderType::Market: return true;
        case OrderType::Limit:
            if (o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                return t.ask <= o.limit_price;
            return t.bid >= o.limit_price;
        case OrderType::Stop:
            if (o.side == OrderSide::Buy || o.side == OrderSide::BuyToCover)
                return t.price >= o.stop_price;
            return t.price <= o.stop_price;
        default: return false;
        }
    }

    // ── Execute a fill ────────────────────────────────────────────────────
    void execute_fill(Order& order, const Bar& bar, double adv, double ann_vol) {
        auto result = slip_.compute_fill(order, bar, adv, ann_vol);

        // Partial fill simulation: large orders vs volume
        double fillable = order.quantity;
        if (adv > 0 && order.quantity > adv * 0.10) {
            // Cap at 10% of ADV per bar (realistic participation rate)
            fillable = std::min(order.quantity, bar.volume * 0.10);
        }

        Fill fill;
        fill.order_id    = order.id;
        fill.instrument  = order.instrument;
        fill.strategy_id = order.strategy_id;
        fill.side        = order.side;
        fill.quantity    = fillable;
        fill.price       = result.fill_price;
        fill.commission  = result.commission;
        fill.slippage    = result.slippage_cost;
        fill.ts          = Clock::instance().fill_time();

        order.filled_qty   += fillable;
        order.avg_fill_price= (order.avg_fill_price * (order.filled_qty - fillable)
                             + result.fill_price * fillable) / order.filled_qty;
        order.commission   += result.commission;
        order.ts_filled     = fill.ts;
        order.status        = (order.filled_qty >= order.quantity - 1e-9)
                              ? OrderStatus::Filled
                              : OrderStatus::PartialFill;

        // IOC: cancel residual
        if (order.type == OrderType::IOC && order.status == OrderStatus::PartialFill)
            order.status = OrderStatus::Cancelled;

        bus_.emit(FillEvent{fill.ts, fill});
        bus_.emit(OrderEvent{fill.ts, order, OrderEvent::Action::Ack});
    }

    EventBus&      bus_;
    SlippageEngine slip_;
    std::atomic<OrderID> order_counter_{0};
    std::unordered_map<InstrumentID, PendingOrders> pending_;
};

} // namespace ql
