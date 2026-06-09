#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// QuantLab — Institutional Quantitative Research Platform
// src/core/Types.h — Canonical type system
//
// Design: All types are value types where possible (cache-friendly).
// Timestamps are nanosecond-precision Unix time (int64_t) — matches
// exchange feed timestamps. Prices use double for research; a production
// system would use fixed-point with explicit tick-size arithmetic.
// ═══════════════════════════════════════════════════════════════════════════
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <chrono>
#include <concepts>
#include <compare>
#include <array>
#include <span>

namespace ql {

// ── Timestamp: nanoseconds since Unix epoch ───────────────────────────────
using Nanos     = std::int64_t;   // nanoseconds
using Micros    = std::int64_t;   // microseconds
using Millis    = std::int64_t;   // milliseconds
using Timestamp = Nanos;

inline constexpr Nanos NS_PER_US  = 1'000LL;
inline constexpr Nanos NS_PER_MS  = 1'000'000LL;
inline constexpr Nanos NS_PER_SEC = 1'000'000'000LL;
inline constexpr Nanos NS_PER_DAY = 86'400LL * NS_PER_SEC;

// ── Instrument identifier ─────────────────────────────────────────────────
// InstrumentID is a compact 64-bit hash; use SymbolRegistry to resolve.
using InstrumentID = std::uint64_t;
using OrderID      = std::uint64_t;
using StrategyID   = std::uint32_t;

// ── Asset class ───────────────────────────────────────────────────────────
enum class AssetClass : std::uint8_t {
    Equity = 0,
    ETF,
    Index,
    Future,
    Option,
    FX,
    Crypto,
    Unknown
};

// ── Exchange ──────────────────────────────────────────────────────────────
enum class Exchange : std::uint8_t {
    NYSE = 0, NASDAQ, ARCA, BATS, IEX,
    CME, CBOE, NYMEX,
    SYNTHETIC,  // for simulated instruments
    UNKNOWN
};

// ── Bar resolution ────────────────────────────────────────────────────────
enum class BarResolution : std::uint32_t {
    Tick    = 0,
    S1      = 1,
    S5      = 5,
    S10     = 10,
    S15     = 15,
    S30     = 30,
    M1      = 60,
    M5      = 300,
    M15     = 900,
    M30     = 1800,
    H1      = 3600,
    H4      = 14400,
    D1      = 86400,
    W1      = 604800,
    Monthly = 2592000,
};

// ── OHLCV Bar ─────────────────────────────────────────────────────────────
struct Bar {
    Timestamp    ts_open   = 0;    // bar open timestamp (ns)
    Timestamp    ts_close  = 0;    // bar close timestamp (ns)
    InstrumentID instrument= 0;
    double       open      = 0.0;
    double       high      = 0.0;
    double       low       = 0.0;
    double       close     = 0.0;
    double       vwap      = 0.0;  // 0 = not computed
    double       volume    = 0.0;
    double       open_interest= 0.0;  // futures/options
    std::uint32_t trade_count = 0;
    BarResolution resolution  = BarResolution::D1;

    // Convenience
    double range()       const noexcept { return high - low; }
    double body()        const noexcept { return close - open; }
    double typical_price()const noexcept{ return (high+low+close)/3.0; }
    bool   is_valid()    const noexcept {
        return open > 0 && high >= open && high >= close
            && low  <= open && low  <= close;
    }

    auto operator<=>(const Bar&) const = default;
};

// ── Tick (trade or quote) ─────────────────────────────────────────────────
struct Tick {
    Timestamp    ts         = 0;
    InstrumentID instrument = 0;
    double       price      = 0.0;
    double       size       = 0.0;
    double       bid        = 0.0;
    double       ask        = 0.0;
    double       bid_size   = 0.0;
    double       ask_size   = 0.0;
    bool         is_trade   = true;  // false = quote update
    bool         aggressor_buy = false;

    double mid()       const noexcept { return (bid+ask)/2.0; }
    double spread()    const noexcept { return ask - bid; }
    double microprice()const noexcept {
        double tot = bid_size + ask_size;
        return (tot > 0) ? (bid*ask_size + ask*bid_size)/tot : mid();
    }
};

// ── Order types ───────────────────────────────────────────────────────────
enum class OrderType : std::uint8_t {
    Market = 0, Limit, Stop, StopLimit,
    IOC,   // Immediate-or-cancel
    FOK,   // Fill-or-kill
    GTC,   // Good-till-cancelled
    MOO,   // Market-on-open
    MOC,   // Market-on-close
    TWAP,  // Time-weighted (algo)
    VWAP,  // Volume-weighted (algo)
};

enum class OrderSide : std::uint8_t { Buy = 0, Sell, SellShort, BuyToCover };
enum class OrderStatus : std::uint8_t {
    New, PartialFill, Filled, Cancelled, Rejected, Expired, PendingCancel
};

enum class TimeInForce : std::uint8_t { Day, GTC, IOC, FOK, GTD };

struct Order {
    OrderID      id           = 0;
    InstrumentID instrument   = 0;
    StrategyID   strategy_id  = 0;
    OrderType    type         = OrderType::Market;
    OrderSide    side         = OrderSide::Buy;
    OrderStatus  status       = OrderStatus::New;
    TimeInForce  tif          = TimeInForce::Day;
    double       quantity     = 0.0;
    double       filled_qty   = 0.0;
    double       limit_price  = 0.0;
    double       stop_price   = 0.0;
    double       avg_fill_price = 0.0;
    double       commission   = 0.0;
    Timestamp    ts_created   = 0;
    Timestamp    ts_sent      = 0;
    Timestamp    ts_filled    = 0;
    Timestamp    ts_cancelled = 0;
    std::uint32_t client_order_id = 0;

    double unfilled() const noexcept { return quantity - filled_qty; }
    bool   is_live()  const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartialFill;
    }
};

// ── Fill ──────────────────────────────────────────────────────────────────
struct Fill {
    OrderID      order_id    = 0;
    InstrumentID instrument  = 0;
    StrategyID   strategy_id = 0;
    OrderSide    side        = OrderSide::Buy;
    double       quantity    = 0.0;
    double       price       = 0.0;
    double       commission  = 0.0;
    double       slippage    = 0.0;   // vs mid at time of order
    Timestamp    ts          = 0;
    Exchange     exchange    = Exchange::UNKNOWN;
};

// ── Position ──────────────────────────────────────────────────────────────
struct Position {
    InstrumentID instrument  = 0;
    StrategyID   strategy_id = 0;
    double       quantity    = 0.0;    // signed: +long, -short
    double       avg_cost    = 0.0;
    double       realized_pnl= 0.0;
    Timestamp    ts_opened   = 0;
    Timestamp    ts_last_fill= 0;

    bool   is_long()  const noexcept { return quantity > 0; }
    bool   is_short() const noexcept { return quantity < 0; }
    bool   is_flat()  const noexcept { return quantity == 0.0; }
    double mkt_value(double px) const noexcept { return quantity * px; }
    double unrealized_pnl(double px) const noexcept {
        return quantity * (px - avg_cost);
    }
};

// ── C++20 Concepts ────────────────────────────────────────────────────────
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template<typename T>
concept PriceType = std::floating_point<T>;

template<typename T>
concept BarLike = requires(T b) {
    { b.open  } -> PriceType;
    { b.high  } -> PriceType;
    { b.low   } -> PriceType;
    { b.close } -> PriceType;
    { b.volume} -> PriceType;
    { b.ts_open } -> std::convertible_to<Timestamp>;
};

template<typename T>
concept TimeSeries = requires(T ts, std::size_t i) {
    { ts[i] }  -> std::convertible_to<double>;
    { ts.size()} -> std::convertible_to<std::size_t>;
};

// ── Signal direction ──────────────────────────────────────────────────────
enum class SignalDirection : std::int8_t { Long = 1, Flat = 0, Short = -1 };

struct Signal {
    InstrumentID instrument  = 0;
    StrategyID   strategy_id = 0;
    Timestamp    ts          = 0;
    double       strength    = 0.0;   // raw signal value
    double       z_score     = 0.0;   // normalized
    SignalDirection direction = SignalDirection::Flat;
    std::uint32_t factor_id  = 0;
};

// ── Sector / classification ───────────────────────────────────────────────
enum class GICSSector : std::uint8_t {
    Energy=0, Materials, Industrials, ConsumerDiscretionary,
    ConsumerStaples, HealthCare, Financials, InformationTechnology,
    CommunicationServices, Utilities, RealEstate, Unknown
};

// ── Return types ──────────────────────────────────────────────────────────
struct ReturnRecord {
    Timestamp ts       = 0;
    double    ret      = 0.0;   // arithmetic return
    double    log_ret  = 0.0;   // log return
};

} // namespace ql
