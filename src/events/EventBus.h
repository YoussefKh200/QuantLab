#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/events/EventBus.h
// Institutional-grade event-driven architecture.
//
// Events are strongly typed and timestamped. The EventBus dispatches
// events to subscribers in timestamp order using a priority queue.
//
// Priority tiers (processed in order within same nanosecond):
//   0 = MarketEvent     (raw data — always first)
//   1 = RiskEvent       (pre-trade risk check)
//   2 = SignalEvent     (alpha signal generated)
//   3 = OrderEvent      (order submitted/modified)
//   4 = FillEvent       (execution confirmed)
//   5 = PortfolioEvent  (position/NAV update)
//   9 = SystemEvent     (session open/close, EOD)
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include <variant>
#include <functional>
#include <queue>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <concepts>
#include <any>

namespace ql {

// ── Event category priorities ──────────────────────────────────────────────
enum class EventPriority : std::uint8_t {
    Market    = 0,
    Risk      = 1,
    Signal    = 2,
    Order     = 3,
    Fill      = 4,
    Portfolio = 5,
    Analytics = 6,
    System    = 9,
};

// ── Concrete event types ───────────────────────────────────────────────────
struct MarketDataEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Market;
    Timestamp    ts;
    InstrumentID instrument;
    Bar          bar;
};

struct TickEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Market;
    Timestamp ts;
    Tick      tick;
};

struct SignalEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Signal;
    Timestamp ts;
    Signal    signal;
};

struct OrderEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Order;
    enum class Action : std::uint8_t { New, Modify, Cancel, Ack, Reject };
    Timestamp ts;
    Order     order;
    Action    action = Action::New;
};

struct FillEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Fill;
    Timestamp ts;
    Fill      fill;
};

struct RiskEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Risk;
    enum class Kind : std::uint8_t { Breach, Warning, Halt, Resume };
    Timestamp   ts;
    Kind        kind;
    std::string message;
    OrderID     related_order = 0;
};

struct PortfolioEvent {
    static constexpr EventPriority PRIORITY = EventPriority::Portfolio;
    enum class Kind : std::uint8_t { PositionUpdate, NAVUpdate, Rebalance };
    Timestamp ts;
    Kind      kind;
    double    nav    = 0;
    double    pnl    = 0;
};

struct SessionEvent {
    static constexpr EventPriority PRIORITY = EventPriority::System;
    enum class Kind : std::uint8_t { PreMarket, Open, Close, AfterHours, EOD, BOD };
    Timestamp ts;
    Kind      kind;
};

// ── Type-erased event wrapper ──────────────────────────────────────────────
using AnyEvent = std::variant<
    MarketDataEvent,
    TickEvent,
    SignalEvent,
    OrderEvent,
    FillEvent,
    RiskEvent,
    PortfolioEvent,
    SessionEvent
>;

struct TimedEvent {
    Timestamp     ts;
    EventPriority priority;
    AnyEvent      event;

    bool operator>(const TimedEvent& o) const noexcept {
        if (ts != o.ts) return ts > o.ts;
        return static_cast<int>(priority) > static_cast<int>(o.priority);
    }
};

// ── Handler concept ────────────────────────────────────────────────────────
template<typename H, typename E>
concept EventHandler = requires(H h, const E& e) {
    { h(e) } -> std::same_as<void>;
};

// ── Event bus ──────────────────────────────────────────────────────────────
class EventBus {
public:
    // Subscribe to a specific event type
    template<typename EventT>
    void subscribe(std::function<void(const EventT&)> handler) {
        auto idx = std::type_index(typeid(EventT));
        handlers_[idx].push_back([h=std::move(handler)](const AnyEvent& ev) {
            h(std::get<EventT>(ev));
        });
    }

    // Publish event immediately (bypasses queue)
    void publish_immediate(AnyEvent ev) {
        dispatch(ev);
    }

    // Enqueue for ordered processing
    void enqueue(AnyEvent ev, Timestamp ts, EventPriority priority) {
        queue_.push({ts, priority, std::move(ev)});
    }

    // Convenience overloads using event's embedded timestamp + priority
    template<typename EventT>
    void emit(EventT ev) {
        Timestamp ts = ev.ts;
        EventPriority pri = EventT::PRIORITY;
        enqueue(std::move(ev), ts, pri);
    }

    // Process all events up to (and including) `until_ts`
    std::size_t process_until(Timestamp until_ts) {
        std::size_t count = 0;
        while (!queue_.empty() && queue_.top().ts <= until_ts) {
            auto te = std::move(const_cast<TimedEvent&>(queue_.top()));
            queue_.pop();
            dispatch(te.event);
            ++count;
        }
        return count;
    }

    // Drain entire queue
    std::size_t drain() {
        return process_until(std::numeric_limits<Timestamp>::max());
    }

    bool empty() const noexcept { return queue_.empty(); }
    std::size_t pending() const noexcept { return queue_.size(); }

    // Next event timestamp
    std::optional<Timestamp> next_ts() const {
        if (queue_.empty()) return std::nullopt;
        return queue_.top().ts;
    }

private:
    void dispatch(const AnyEvent& ev) {
        std::visit([&](auto&& concrete_ev) {
            auto idx = std::type_index(typeid(decltype(concrete_ev)));
            auto it  = handlers_.find(idx);
            if (it != handlers_.end())
                for (auto& h : it->second) h(ev);
        }, ev);
    }

    using HandlerFn = std::function<void(const AnyEvent&)>;
    std::unordered_map<std::type_index, std::vector<HandlerFn>> handlers_;
    std::priority_queue<TimedEvent,
                        std::vector<TimedEvent>,
                        std::greater<TimedEvent>> queue_;
};

} // namespace ql
