#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/data/DataPipeline.h
// Parallel enriched-bar preprocessing pipeline.
// Ported from QuantEngine v2, fully integrated into ql:: namespace.
//
// EnrichedBar adds pre-computed technical features to raw OHLCV:
//   VWAP (daily-reset), ATR(14), RSI(14) Wilder, Bollinger Bands (20,2σ),
//   relative volume (vs 20-bar avg)
//
// ParallelDataPipeline: dispatches N symbol jobs to ThreadPool,
// returns vector<PipelineResult> with raw + enriched bars.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/ThreadPool.h"
#include "DataManager.h"
#include <deque>
#include <vector>
#include <string>
#include <future>
#include <cmath>
#include <algorithm>

namespace ql {

// ── Enriched bar: raw OHLCV + pre-computed indicators ─────────────────────
struct EnrichedBar {
    Bar    bar;
    double log_return   = 0.0;  // ln(close_t / close_{t-1})
    double vwap         = 0.0;  // running daily VWAP (resets on day boundary)
    double atr14        = 0.0;  // ATR(14) — true range average
    double rsi14        = 0.0;  // RSI(14) Wilder smoothing
    double bb_upper     = 0.0;  // Bollinger upper band (20, 2σ)
    double bb_lower     = 0.0;  // Bollinger lower band
    double bb_pct       = 0.0;  // (close - lower) / (upper - lower) ∈ [0,1]
    double volume_ratio = 0.0;  // bar_volume / 20-bar avg volume
};

// ── Per-symbol preprocessing ───────────────────────────────────────────────
inline std::vector<EnrichedBar> preprocess_bars(const std::vector<Bar>& bars) {
    std::vector<EnrichedBar> out;
    out.reserve(bars.size());

    constexpr int RSI_P = 14, BB_P = 20, ATR_P = 14, VOL_P = 20;

    std::deque<double> closes, volumes, trs;
    double cum_pv = 0, cum_v = 0;
    double avg_gain = 0, avg_loss = 0;
    bool rsi_seeded = false;
    double prev_close = bars.empty() ? 0 : bars[0].close;

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const Bar& b = bars[i];
        EnrichedBar eb;
        eb.bar = b;

        // ── Log return ───────────────────────────────────────────────────
        if (i > 0 && prev_close > 0)
            eb.log_return = std::log(b.close / prev_close);

        // ── VWAP (resets at day boundary) ─────────────────────────────────
        Timestamp day      = b.ts_open  / NS_PER_DAY;
        Timestamp prev_day = (i > 0) ? bars[i-1].ts_open / NS_PER_DAY : day;
        if (day != prev_day) { cum_pv = 0; cum_v = 0; }
        double typical = b.typical_price();
        cum_pv += typical * b.volume;
        cum_v  += b.volume;
        eb.vwap = (cum_v > 0) ? cum_pv / cum_v : typical;

        // ── True Range + ATR(14) ─────────────────────────────────────────
        double tr = b.range();
        if (i > 0) tr = std::max({tr,
            std::abs(b.high - prev_close),
            std::abs(b.low  - prev_close)});
        trs.push_back(tr);
        if ((int)trs.size() > ATR_P) trs.pop_front();
        if ((int)trs.size() == ATR_P) {
            double s = 0; for (double v : trs) s += v;
            eb.atr14 = s / ATR_P;
        }

        // ── RSI(14) Wilder ────────────────────────────────────────────────
        if (i > 0) {
            double delta = b.close - prev_close;
            double gain  = std::max(delta,  0.0);
            double loss  = std::max(-delta, 0.0);
            if (!rsi_seeded) {
                avg_gain = gain; avg_loss = loss; rsi_seeded = true;
            } else {
                avg_gain = (avg_gain * (RSI_P-1) + gain) / RSI_P;
                avg_loss = (avg_loss * (RSI_P-1) + loss) / RSI_P;
                if (avg_loss > 0) {
                    double rs = avg_gain / avg_loss;
                    eb.rsi14 = 100.0 - (100.0 / (1.0 + rs));
                } else {
                    eb.rsi14 = 100.0;
                }
            }
        }

        // ── Bollinger Bands (20, 2σ) ──────────────────────────────────────
        closes.push_back(b.close);
        if ((int)closes.size() > BB_P) closes.pop_front();
        if ((int)closes.size() == BB_P) {
            double mu = 0; for (double c : closes) mu += c; mu /= BB_P;
            double var = 0; for (double c : closes) var += (c-mu)*(c-mu);
            double sd = std::sqrt(var / BB_P);
            eb.bb_upper = mu + 2*sd;
            eb.bb_lower = mu - 2*sd;
            double rng  = eb.bb_upper - eb.bb_lower;
            eb.bb_pct   = (rng > 0) ? (b.close - eb.bb_lower) / rng : 0.5;
        }

        // ── Relative volume ───────────────────────────────────────────────
        volumes.push_back(b.volume);
        if ((int)volumes.size() > VOL_P) volumes.pop_front();
        if ((int)volumes.size() == VOL_P) {
            double avg = 0; for (double v : volumes) avg += v; avg /= VOL_P;
            eb.volume_ratio = (avg > 0) ? b.volume / avg : 1.0;
        }

        prev_close = b.close;
        out.push_back(eb);
    }
    return out;
}

// ── Pipeline result ────────────────────────────────────────────────────────
struct PipelineResult {
    std::string                symbol;
    std::shared_ptr<DataFeed>  feed;             // raw DataFeed in cache
    std::vector<EnrichedBar>   enriched;
    std::string                error;
};

// ── Parallel data pipeline ─────────────────────────────────────────────────
class DataPipeline {
public:
    explicit DataPipeline(DataManager& dm = data_mgr()) : dm_(dm) {}

    // Add a pre-loaded DataFeed for enrichment
    void add_feed(std::shared_ptr<DataFeed> feed) {
        pending_.push_back(std::move(feed));
    }

    // Add a CSV file to load + enrich
    void add_csv(const std::string& path, const std::string& ticker,
                 CsvSchema schema = {}) {
        csv_jobs_.push_back({path, ticker, schema});
    }

    // Generate synthetic + enrich
    void add_synthetic(const std::string& ticker,
                       Timestamp start, int n_bars,
                       double price = 100.0,
                       double vol   = 0.20,
                       double ret   = 0.10,
                       unsigned seed= 42,
                       BarResolution res = BarResolution::D1) {
        auto feed = dm_.generate_synthetic(ticker, start, n_bars,
                                           price, vol, ret, seed, res);
        pending_.push_back(std::move(feed));
    }

    // Execute all jobs in parallel; return results
    std::vector<PipelineResult> run() {
        // Load CSV jobs first (also async)
        for (auto& [path, ticker, schema] : csv_jobs_) {
            auto fut = global_pool().submit([this, path, ticker, schema]() mutable {
                return dm_.load_csv(path, ticker, schema);
            });
            csv_futures_.push_back(std::move(fut));
        }
        for (auto& fut : csv_futures_) {
            try { pending_.push_back(fut.get()); }
            catch (const std::exception& e) {
                PipelineResult r; r.error = e.what();
                results_.push_back(std::move(r));
            }
        }
        csv_futures_.clear(); csv_jobs_.clear();

        // Enrich all feeds in parallel
        std::vector<std::future<PipelineResult>> futs;
        for (auto& feed : pending_) {
            futs.push_back(global_pool().submit([feed]() mutable {
                PipelineResult r;
                r.symbol   = SymbolRegistry::instance().ticker_of(feed->instrument());
                r.feed     = feed;
                r.enriched = preprocess_bars(feed->bars());
                return r;
            }));
        }
        for (auto& fut : futs) {
            try { results_.push_back(fut.get()); }
            catch (const std::exception& e) {
                PipelineResult r; r.error = e.what();
                results_.push_back(std::move(r));
            }
        }
        pending_.clear();

        return results_;
    }

    // Collect just the DataFeeds for passing to Backtester
    std::vector<std::shared_ptr<DataFeed>> feeds() const {
        std::vector<std::shared_ptr<DataFeed>> v;
        for (auto& r : results_) if (r.feed) v.push_back(r.feed);
        return v;
    }

    std::size_t thread_count() const { return global_pool().thread_count(); }
    void clear() { results_.clear(); pending_.clear(); }

private:
    struct CsvJob { std::string path, ticker; CsvSchema schema; };

    DataManager& dm_;
    std::vector<std::shared_ptr<DataFeed>>  pending_;
    std::vector<CsvJob>                     csv_jobs_;
    std::vector<std::future<std::shared_ptr<DataFeed>>> csv_futures_;
    std::vector<PipelineResult>             results_;
};

} // namespace ql
