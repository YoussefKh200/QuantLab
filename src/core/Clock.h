#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/core/Clock.h
// Deterministic simulation clock.
//
// In live trading: wraps std::chrono::system_clock
// In simulation: advances only when explicitly ticked by the event engine.
// All latency (network, exchange, execution) is simulated by advancing
// the clock by configurable delays — ensuring deterministic replay.
// ═══════════════════════════════════════════════════════════════════════════
#include "Types.h"
#include <atomic>
#include <functional>
#include <vector>
#include <algorithm>
#include <chrono>

namespace ql {

// ── Clock mode ─────────────────────────────────────────────────────────────
enum class ClockMode { Simulation, Live };

// ── Timer callback ─────────────────────────────────────────────────────────
struct TimerEvent {
    Timestamp                fire_at;
    std::function<void()>    callback;
    bool                     recurring = false;
    Nanos                    interval  = 0;

    bool operator>(const TimerEvent& o) const noexcept {
        return fire_at > o.fire_at;
    }
};

// ── Simulation clock ───────────────────────────────────────────────────────
class Clock {
public:
    static Clock& instance() {
        static Clock clk;
        return clk;
    }

    void set_mode(ClockMode m) noexcept { mode_ = m; }
    ClockMode mode() const noexcept { return mode_; }

    // Current time
    Timestamp now() const noexcept {
        if (mode_ == ClockMode::Live) {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
        }
        return sim_time_.load(std::memory_order_acquire);
    }

    // Advance simulation clock (only valid in simulation mode)
    void advance_to(Timestamp ts) noexcept {
        if (mode_ != ClockMode::Simulation) return;
        Timestamp current = sim_time_.load(std::memory_order_acquire);
        if (ts > current)
            sim_time_.store(ts, std::memory_order_release);
        fire_timers(ts);
    }

    void advance_by(Nanos delta) noexcept {
        advance_to(now() + delta);
    }

    void reset(Timestamp start = 0) noexcept {
        sim_time_.store(start, std::memory_order_release);
        timers_.clear();
    }

    // ── Latency simulation ─────────────────────────────────────────────────
    struct LatencyConfig {
        Nanos network_one_way    = 100'000LL;     // 100µs one-way
        Nanos exchange_processing= 5'000LL;        // 5µs exchange matching
        Nanos execution_engine   = 50'000LL;       // 50µs internal OMS
        Nanos market_data_feed   = 200'000LL;      // 200µs MD latency
    };

    void set_latency(LatencyConfig cfg) noexcept { latency_ = cfg; }
    const LatencyConfig& latency() const noexcept { return latency_; }

    Timestamp order_ack_time() const noexcept {
        return now() + latency_.execution_engine + latency_.network_one_way;
    }
    Timestamp fill_time() const noexcept {
        return order_ack_time() + latency_.exchange_processing
                                + latency_.network_one_way;
    }
    Timestamp md_receive_time() const noexcept {
        return now() + latency_.market_data_feed;
    }

    // ── Timers ─────────────────────────────────────────────────────────────
    void schedule(Timestamp fire_at, std::function<void()> cb,
                  bool recurring = false, Nanos interval = 0) {
        timers_.push_back({fire_at, std::move(cb), recurring, interval});
        std::push_heap(timers_.begin(), timers_.end(),
                       std::greater<TimerEvent>{});
    }

    // ── Date/time helpers ──────────────────────────────────────────────────
    static Timestamp from_date(int year, int month, int day,
                                int hour=0, int min=0, int sec=0) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon  = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min  = min;
        tm.tm_sec  = sec;
#ifdef _WIN32
        auto t = _mkgmtime(&tm);
#else
        auto t = timegm(&tm);
#endif
        return static_cast<Timestamp>(t) * NS_PER_SEC;
    }

    static std::string to_string(Timestamp ts) {
        time_t t = ts / NS_PER_SEC;
        char buf[32];
        struct tm* tm_p = gmtime(&t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_p);
        return std::string(buf);
    }

private:
    Clock() : mode_(ClockMode::Simulation), sim_time_(0) {}

    void fire_timers(Timestamp ts) {
        while (!timers_.empty() && timers_.front().fire_at <= ts) {
            std::pop_heap(timers_.begin(), timers_.end(),
                          std::greater<TimerEvent>{});
            auto ev = std::move(timers_.back());
            timers_.pop_back();
            ev.callback();
            if (ev.recurring && ev.interval > 0)
                schedule(ts + ev.interval, ev.callback, true, ev.interval);
        }
    }

    ClockMode           mode_;
    std::atomic<Nanos>  sim_time_;
    LatencyConfig       latency_;
    std::vector<TimerEvent> timers_;
};

// ── Convenience ───────────────────────────────────────────────────────────
inline Timestamp now_ns() { return Clock::instance().now(); }

} // namespace ql

#include <ctime>
