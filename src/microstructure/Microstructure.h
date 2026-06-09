#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/microstructure/Microstructure.h
// Market microstructure analytics.
//
// Implements institutional-grade microstructure metrics:
//   - Realized spread / effective spread
//   - Trade classification (Lee-Ready, BVC)
//   - Volume-synchronized probability of informed trading (VPIN)
//   - Order flow imbalance (OFI)
//   - Kyle's lambda (price impact coefficient)
//   - Amihud illiquidity ratio
//   - Corwin-Schultz spread estimator (from OHLC)
//   - Roll spread estimator
//   - Microprice (volume-weighted mid)
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include <deque>
#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <optional>

namespace ql {
namespace micro {

// ── Trade sign classifier (Lee-Ready algorithm) ────────────────────────────
// Returns +1 (buy-initiated) or -1 (sell-initiated)
inline int classify_trade_lr(double trade_price, double mid, double prev_mid) {
    if (trade_price > mid)           return +1;  // above mid = buy
    if (trade_price < mid)           return -1;  // below mid = sell
    // Tick rule: compare to previous mid
    if (mid > prev_mid)              return +1;
    if (mid < prev_mid)              return -1;
    return 0;
}

// ── Bulk volume classification (BVC, Easley et al.) ───────────────────────
inline double bvc_buy_fraction(double ret, double sigma) {
    // Fraction of volume that's buyer-initiated
    if (sigma < 1e-12) return 0.5;
    double z = ret / sigma;
    // Approximation of normal CDF
    auto phi = [](double x) -> double {
        return 0.5 * std::erfc(-x / std::sqrt(2.0));
    };
    return phi(z);
}

// ── Rolling microstructure calculator ─────────────────────────────────────
struct MicroConfig {
    int    vpin_bucket_size  = 50;
    int    vpin_window       = 50;
    int    ofi_window        = 20;
    int    impact_window     = 60;
    int    amihud_window     = 252;
    int    roll_window       = 10;
    double sigma_window      = 20;
};

class MicrostructureEngine {
public:
    explicit MicrostructureEngine(MicroConfig cfg = MicroConfig()) : cfg_(cfg) {}

    struct Snapshot {
        // Spread estimates
        double effective_spread    = 0;   // 2 * |trade_price - mid|
        double realized_spread     = 0;   // 2 * sign * (trade - mid_t+5)
        double quoted_spread       = 0;   // ask - bid
        double roll_spread         = 0;   // Roll (1984) estimator
        double cs_spread           = 0;   // Corwin-Schultz spread

        // Price impact
        double kyle_lambda         = 0;   // price impact per unit volume
        double amihud_illiquidity  = 0;   // |ret| / volume

        // Order flow
        double ofi                 = 0;   // order flow imbalance
        double vpin                = 0;   // VPIN (0-1)
        double buy_volume_frac     = 0;   // fraction buyer-initiated

        // Microprice
        double microprice          = 0;
        double mid                 = 0;
        double bid                 = 0;
        double ask                 = 0;

        // Liquidity
        double depth_imbalance     = 0;   // (bid_sz - ask_sz)/(bid_sz+ask_sz)
    };

    // Update with a new tick (quote or trade)
    Snapshot update_tick(const Tick& t) {
        if (t.is_trade) {
            trades_.push_back(t);
            if ((int)trades_.size() > cfg_.impact_window * 10)
                trades_.pop_front();
            update_vpin(t);
        } else {
            quotes_.push_back(t);
            if ((int)quotes_.size() > cfg_.ofi_window * 2)
                quotes_.pop_front();
        }

        Snapshot s;
        s.mid         = t.mid();
        s.bid         = t.bid;
        s.ask         = t.ask;
        s.microprice  = t.microprice();
        s.quoted_spread = t.spread();

        if (t.is_trade && !quotes_.empty()) {
            double sign = (int)t.aggressor_buy ? 1.0 : -1.0;
            s.effective_spread = 2.0 * sign * (t.price - t.mid());
        }

        s.ofi             = compute_ofi();
        s.vpin            = current_vpin_;
        s.buy_volume_frac = buy_vol_frac_;
        s.kyle_lambda     = compute_kyle_lambda();
        s.amihud_illiquidity = current_amihud_;
        s.depth_imbalance = (t.bid_size + t.ask_size > 0)
            ? (t.bid_size - t.ask_size) / (t.bid_size + t.ask_size) : 0;

        return s;
    }

    // Update with a bar (when tick data unavailable)
    Snapshot update_bar(const Bar& bar) {
        bars_.push_back(bar);
        if ((int)bars_.size() > std::max(cfg_.amihud_window, cfg_.roll_window) + 5)
            bars_.pop_front();

        Snapshot s;
        s.mid = bar.typical_price();

        // Corwin-Schultz spread (log-price based)
        s.cs_spread   = corwin_schultz_spread();
        // Roll spread
        s.roll_spread = roll_spread();
        // Amihud
        s.amihud_illiquidity = amihud_ratio();
        // BVC buy fraction
        if (bars_.size() >= 2) {
            double ret = std::log(bar.close / bars_[bars_.size()-2].close);
            double sig = realized_vol(20) / std::sqrt(252.0);
            s.buy_volume_frac = bvc_buy_fraction(ret, sig);
        }

        return s;
    }

private:
    // ── VPIN computation ─────────────────────────────────────────────────
    void update_vpin(const Tick& t) {
        double buy_vol = t.aggressor_buy ? t.size : 0;
        double sell_vol= t.aggressor_buy ? 0 : t.size;

        current_bucket_buy_  += buy_vol;
        current_bucket_sell_ += sell_vol;
        current_bucket_vol_  += t.size;

        if (current_bucket_vol_ >= cfg_.vpin_bucket_size) {
            double imbalance = std::abs(current_bucket_buy_ - current_bucket_sell_)
                             / current_bucket_vol_;
            vpin_buckets_.push_back(imbalance);
            if ((int)vpin_buckets_.size() > cfg_.vpin_window)
                vpin_buckets_.pop_front();

            // VPIN = average absolute order flow imbalance
            current_vpin_ = 0;
            for (double v : vpin_buckets_) current_vpin_ += v;
            current_vpin_ /= vpin_buckets_.size();

            double total = current_bucket_buy_ + current_bucket_sell_;
            buy_vol_frac_ = (total > 0) ? current_bucket_buy_ / total : 0.5;

            current_bucket_buy_ = current_bucket_sell_ = current_bucket_vol_ = 0;
        }
    }

    // ── Order flow imbalance ──────────────────────────────────────────────
    double compute_ofi() const {
        if (quotes_.size() < 2) return 0;
        double ofi = 0;
        for (std::size_t i = 1; i < quotes_.size(); ++i) {
            const auto& prev = quotes_[i-1];
            const auto& curr = quotes_[i];
            // OFI = ΔBidSize_if_bid_unchanged_or_improved - ΔAskSize_if_ask_unchanged_or_improved
            double bid_contrib  = (curr.bid >= prev.bid) ? curr.bid_size : -prev.bid_size;
            double ask_contrib  = (curr.ask <= prev.ask) ?-curr.ask_size :  prev.ask_size;
            ofi += bid_contrib + ask_contrib;
        }
        return ofi / quotes_.size();
    }

    // ── Kyle's lambda (OLS of price change on signed volume) ─────────────
    double compute_kyle_lambda() const {
        if ((int)trades_.size() < 10) return 0;
        int n = std::min((int)trades_.size(), cfg_.impact_window);
        // Compute signed volume and price changes
        std::vector<double> x, y;
        for (int i = 1; i <= n && i < (int)trades_.size(); ++i) {
            const auto& t = trades_[trades_.size()-i];
            double sign = t.aggressor_buy ? 1.0 : -1.0;
            x.push_back(sign * t.size);
            if (i > 1) {
                const auto& prev = trades_[trades_.size()-i+1];
                y.push_back(t.price - prev.price);
            }
        }
        if (x.size() < 5 || y.size() < 5) return 0;
        // Simple OLS slope
        int m = (int)y.size();
        double sx=0, sy=0, sxy=0, sxx=0;
        for (int i = 0; i < m; ++i) { sx+=x[i]; sy+=y[i]; sxy+=x[i]*y[i]; sxx+=x[i]*x[i]; }
        double denom = m*sxx - sx*sx;
        return (std::abs(denom) > 1e-12) ? (m*sxy - sx*sy)/denom : 0;
    }

    // ── Amihud illiquidity ratio ──────────────────────────────────────────
    double amihud_ratio() const {
        if ((int)bars_.size() < 5) return 0;
        int n = std::min((int)bars_.size(), cfg_.amihud_window);
        double sum = 0; int cnt = 0;
        for (int i = (int)bars_.size()-n; i < (int)bars_.size()-1; ++i) {
            const auto& b = bars_[i];
            if (b.volume < 1) continue;
            double ret = std::abs(std::log(b.close / bars_[i-1 > 0 ? i-1 : i].close));
            sum += ret / (b.volume * b.close / 1e6); // per $1M
            ++cnt;
        }
        current_amihud_ = (cnt > 0) ? sum / cnt : 0;
        return current_amihud_;
    }

    // ── Roll spread estimator: 2*sqrt(-cov(ΔP_t, ΔP_{t-1})) ─────────────
    double roll_spread() const {
        if ((int)bars_.size() < cfg_.roll_window + 2) return 0;
        int n = cfg_.roll_window;
        std::vector<double> dp;
        int start = (int)bars_.size() - n - 1;
        for (int i = start+1; i < (int)bars_.size(); ++i)
            dp.push_back(bars_[i].close - bars_[i-1].close);
        if ((int)dp.size() < 2) return 0;
        double cov = 0;
        for (int i = 1; i < (int)dp.size(); ++i)
            cov += dp[i] * dp[i-1];
        cov /= dp.size()-1;
        return (cov < 0) ? 2.0 * std::sqrt(-cov) : 0;
    }

    // ── Corwin-Schultz spread (log H/L based) ─────────────────────────────
    double corwin_schultz_spread() const {
        if ((int)bars_.size() < 2) return 0;
        int n = std::min((int)bars_.size(), 20);
        double sum = 0; int cnt = 0;
        for (int i = (int)bars_.size()-n; i < (int)bars_.size()-1; ++i) {
            const auto& b0 = bars_[i], &b1 = bars_[i+1];
            double h2 = std::log(std::max(b0.high,b1.high) / std::min(b0.low,b1.low));
            double h  = std::log(b0.high/b0.low);
            double k  = h2*h2 / (8*std::log(2.0)) - h*h / (4*std::log(2.0));
            double alpha = (std::sqrt(2*k) - std::sqrt(k)) / (3 - 2*std::sqrt(2.0));
            double spread = 2*(std::exp(alpha) - 1) / (1 + std::exp(alpha));
            if (spread > 0) { sum += spread; ++cnt; }
        }
        return (cnt > 0) ? sum / cnt : 0;
    }

    // ── Realized volatility ────────────────────────────────────────────────
    double realized_vol(int window) const {
        if ((int)bars_.size() < window+1) return 0;
        double var = 0;
        int start = (int)bars_.size()-window-1;
        for (int i = start+1; i < (int)bars_.size(); ++i) {
            double r = std::log(bars_[i].close / bars_[i-1].close);
            var += r*r;
        }
        return std::sqrt(var / window * 252.0);
    }

    MicroConfig cfg_;
    std::deque<Tick>  trades_, quotes_;
    std::deque<Bar>   bars_;

    // VPIN state
    std::deque<double> vpin_buckets_;
    double current_bucket_buy_  = 0;
    double current_bucket_sell_ = 0;
    double current_bucket_vol_  = 0;
    double current_vpin_        = 0;
    double buy_vol_frac_        = 0.5;
    mutable double current_amihud_ = 0;
};

} // namespace micro
} // namespace ql
