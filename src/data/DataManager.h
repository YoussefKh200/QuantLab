#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/data/DataManager.h
// Central data access layer.
//
// Responsibilities:
//   - Load from CSV / binary formats
//   - Adjust for splits and dividends
//   - Survivor-bias mitigation via universe snapshots
//   - LRU cache for hot data
//   - Async parallel loading via ThreadPool
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/SymbolRegistry.h"
#include "../core/Clock.h"
#include "../core/ThreadPool.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <mutex>
#include <list>
#include <optional>
#include <functional>
#include <stdexcept>
#include <cmath>
#include <iomanip>
#include <future>
#include <random>
#include <span>

namespace ql {
namespace fs = std::filesystem;

// ── Corporate action ────────────────────────────────────────────────────────
struct CorporateAction {
    enum class Type : std::uint8_t { Split, Dividend, SpinOff, Merger };
    InstrumentID instrument;
    Timestamp    ex_date;
    Type         type;
    double       factor;    // split ratio or dividend per share
};

// ── Bar cache key ────────────────────────────────────────────────────────────
struct CacheKey {
    InstrumentID instrument;
    BarResolution resolution;
    bool operator==(const CacheKey& o) const noexcept {
        return instrument == o.instrument && resolution == o.resolution;
    }
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
        return k.instrument ^ (static_cast<std::size_t>(k.resolution) << 32);
    }
};

// ── DataFeed: single-instrument bar sequence ──────────────────────────────
class DataFeed {
public:
    DataFeed(InstrumentID id, BarResolution res, std::vector<Bar> bars)
        : instrument_(id), resolution_(res), bars_(std::move(bars)) {
        // Ensure sorted
        std::sort(bars_.begin(), bars_.end(),
                  [](const Bar& a, const Bar& b){ return a.ts_open < b.ts_open; });
    }

    // Iterate bars in time window
    std::span<const Bar> range(Timestamp from, Timestamp to) const {
        auto first = std::lower_bound(bars_.begin(), bars_.end(), from,
            [](const Bar& b, Timestamp t){ return b.ts_open < t; });
        auto last  = std::upper_bound(bars_.begin(), bars_.end(), to,
            [](Timestamp t, const Bar& b){ return t < b.ts_open; });
        return std::span<const Bar>{first, last};
    }

    const Bar* bar_at(Timestamp ts) const {
        auto it = std::lower_bound(bars_.begin(), bars_.end(), ts,
            [](const Bar& b, Timestamp t){ return b.ts_open < t; });
        if (it == bars_.end() || it->ts_open != ts) return nullptr;
        return &(*it);
    }

    // Rolling window access
    std::vector<double> close_prices(Timestamp from, Timestamp to) const {
        auto sp = range(from, to);
        std::vector<double> v;
        v.reserve(sp.size());
        for (auto& b : sp) v.push_back(b.close);
        return v;
    }

    InstrumentID instrument()   const noexcept { return instrument_; }
    BarResolution resolution()  const noexcept { return resolution_; }
    const std::vector<Bar>& bars() const noexcept { return bars_; }
    std::size_t size()          const noexcept { return bars_.size(); }
    bool empty()                const noexcept { return bars_.empty(); }
    Timestamp start()           const noexcept { return bars_.empty() ? 0 : bars_.front().ts_open; }
    Timestamp end()             const noexcept { return bars_.empty() ? 0 : bars_.back().ts_open; }

    // Apply adjustment factor to all prices (split/dividend)
    void adjust(double factor) {
        for (auto& b : bars_) {
            b.open  /= factor;
            b.high  /= factor;
            b.low   /= factor;
            b.close /= factor;
            b.vwap  /= factor;
            b.volume *= factor;
        }
    }

private:
    InstrumentID  instrument_;
    BarResolution resolution_;
    std::vector<Bar> bars_;
};

// ── CSV schema ─────────────────────────────────────────────────────────────
struct CsvSchema {
    int  col_date   = 0;
    int  col_open   = 1;
    int  col_high   = 2;
    int  col_low    = 3;
    int  col_close  = 4;
    int  col_volume = 5;
    int  col_vwap   = -1;    // -1 = not present
    char delimiter  = ',';
    bool has_header = true;
    BarResolution resolution = BarResolution::D1;
};

// ── Data manager ────────────────────────────────────────────────────────────
class DataManager {
public:
    explicit DataManager(std::size_t cache_capacity_mb = 512)
        : cache_cap_(cache_capacity_mb * 1024 * 1024 / sizeof(Bar)) {}

    // ── CSV ingestion ──────────────────────────────────────────────────────
    std::shared_ptr<DataFeed> load_csv(const fs::path& path,
                                        const std::string& ticker,
                                        CsvSchema schema = {}) {
        InstrumentID id = SymbolRegistry::instance().get_or_create(ticker);
        auto bars = parse_csv(path, id, schema);
        apply_corporate_actions(id, bars);
        auto feed = std::make_shared<DataFeed>(id, schema.resolution,
                                               std::move(bars));
        cache_put({id, schema.resolution}, feed);
        return feed;
    }

    // ── Parallel multi-asset loading ───────────────────────────────────────
    std::vector<std::shared_ptr<DataFeed>> load_csv_parallel(
        const std::vector<std::pair<fs::path, std::string>>& files,
        CsvSchema schema = {})
    {
        std::vector<std::future<std::shared_ptr<DataFeed>>> futures;
        for (auto& [path, ticker] : files) {
            futures.push_back(global_pool().submit(
                [this, path, ticker, schema]() mutable {
                    return load_csv(path, ticker, schema);
                }));
        }
        std::vector<std::shared_ptr<DataFeed>> results;
        results.reserve(futures.size());
        for (auto& f : futures) results.push_back(f.get());
        return results;
    }

    // ── Cache access ───────────────────────────────────────────────────────
    std::shared_ptr<DataFeed> get(InstrumentID id, BarResolution res) {
        return cache_get({id, res});
    }

    std::shared_ptr<DataFeed> get(std::string_view ticker, BarResolution res) {
        auto id_opt = SymbolRegistry::instance().find_id(ticker);
        if (!id_opt) return nullptr;
        return get(*id_opt, res);
    }

    // ── Corporate actions ──────────────────────────────────────────────────
    void add_corporate_action(CorporateAction ca) {
        std::lock_guard lk(mu_);
        corp_actions_[ca.instrument].push_back(ca);
        // Sort by ex_date
        auto& v = corp_actions_[ca.instrument];
        std::sort(v.begin(), v.end(),
                  [](auto& a, auto& b){ return a.ex_date < b.ex_date; });
    }

    // ── Survivorship bias: universe snapshot ───────────────────────────────
    // Returns set of instruments listed at given timestamp
    std::vector<InstrumentID> universe_at(Timestamp ts) const {
        auto all = SymbolRegistry::instance().all_ids();
        std::vector<InstrumentID> result;
        for (auto id : all) {
            auto* meta = SymbolRegistry::instance().get_metadata(id);
            if (meta && meta->is_listed(ts)) result.push_back(id);
        }
        return result;
    }

    // ── Synthetic data generation (for testing) ────────────────────────────
    std::shared_ptr<DataFeed> generate_synthetic(
        const std::string& ticker,
        Timestamp start, int n_bars,
        double init_price = 100.0,
        double annual_vol = 0.20,
        double annual_ret = 0.10,
        unsigned seed = 42,
        BarResolution res = BarResolution::D1)
    {
        InstrumentID id = SymbolRegistry::instance().get_or_create(ticker);
        auto bars = synthesize_bars(id, start, n_bars, init_price,
                                    annual_vol, annual_ret, seed, res);
        auto feed = std::make_shared<DataFeed>(id, res, std::move(bars));
        cache_put({id, res}, feed);
        return feed;
    }

private:
    // ── CSV parser ─────────────────────────────────────────────────────────
    static std::vector<Bar> parse_csv(const fs::path& path,
                                       InstrumentID id,
                                       const CsvSchema& schema) {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open: " + path.string());

        std::vector<Bar> bars;
        bars.reserve(1 << 20);
        std::string line;
        if (schema.has_header) std::getline(f, line);

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split_csv(line, schema.delimiter);
            int max_col = std::max({schema.col_date, schema.col_open,
                                    schema.col_high, schema.col_low,
                                    schema.col_close, schema.col_volume});
            if ((int)cols.size() <= max_col) continue;

            Bar b;
            b.instrument = id;
            b.resolution = schema.resolution;
            b.ts_open    = parse_date(trim(cols[schema.col_date]));
            b.ts_close   = b.ts_open + static_cast<Nanos>(schema.resolution)
                                     * NS_PER_SEC;
            try {
                b.open   = std::stod(cols[schema.col_open]);
                b.high   = std::stod(cols[schema.col_high]);
                b.low    = std::stod(cols[schema.col_low]);
                b.close  = std::stod(cols[schema.col_close]);
                b.volume = schema.col_volume >= 0 && (int)cols.size() > schema.col_volume
                           ? std::stod(cols[schema.col_volume]) : 0;
                b.vwap   = schema.col_vwap >= 0 && (int)cols.size() > schema.col_vwap
                           ? std::stod(cols[schema.col_vwap]) : b.typical_price();
                if (b.is_valid()) bars.push_back(b);
            } catch (...) { /* skip malformed rows */ }
        }
        std::sort(bars.begin(), bars.end(),
                  [](const Bar& a, const Bar& b){ return a.ts_open < b.ts_open; });
        return bars;
    }

    // ── Corporate action adjustment (backward-adjust: latest prices raw) ───
    void apply_corporate_actions(InstrumentID id, std::vector<Bar>& bars) {
        std::lock_guard lk(mu_);
        auto it = corp_actions_.find(id);
        if (it == corp_actions_.end()) return;

        for (auto& ca : it->second) {
            if (ca.type != CorporateAction::Type::Split &&
                ca.type != CorporateAction::Type::Dividend) continue;
            // Adjust all bars before ex_date
            for (auto& b : bars) {
                if (b.ts_open >= ca.ex_date) continue;
                b.open   /= ca.factor;
                b.high   /= ca.factor;
                b.low    /= ca.factor;
                b.close  /= ca.factor;
                b.vwap   /= ca.factor;
                b.volume *= ca.factor;
            }
        }
    }

    // ── Synthetic GBM + regime switching ──────────────────────────────────
    static std::vector<Bar> synthesize_bars(
        InstrumentID id, Timestamp start, int n,
        double p0, double av, double ar,
        unsigned seed, BarResolution res)
    {
        std::mt19937_64 rng(seed);
        std::normal_distribution<double> N(0,1);
        std::uniform_real_distribution<double> U(0,1);

        long bar_ns = static_cast<long>(res) * NS_PER_SEC;
        double dt   = static_cast<double>(res) / 86400.0 / 252.0;
        double mu   = ar, sig = av;
        double p    = p0;
        bool bear   = false;

        std::vector<Bar> bars;
        bars.reserve(n);

        for (int i = 0; i < n; ++i) {
            if (U(rng) < 0.02) bear = !bear;
            double cm = mu  + (bear ? -0.3 : 0);
            double cs = sig * (bear ?  2.0 : 1.0);

            double ret  = (cm - 0.5*cs*cs)*dt + cs*std::sqrt(dt)*N(rng);
            double close= p * std::exp(ret);
            double iv   = cs * std::sqrt(dt) * p * 0.4;
            double open = p * std::exp((cm-0.5*cs*cs)*dt*0.3 + cs*std::sqrt(dt)*0.3*N(rng));
            double high = std::max({open,close}) + std::abs(N(rng))*iv;
            double low  = std::min({open,close}) - std::abs(N(rng))*iv;
            low  = std::max(low, 0.01);
            double vol  = 1e6 * (1+0.5*std::abs(N(rng)));

            Bar b;
            b.instrument = id;
            b.resolution = res;
            b.ts_open    = start + static_cast<Timestamp>(i) * bar_ns;
            b.ts_close   = b.ts_open + bar_ns;
            b.open = open; b.high = high; b.low = low; b.close = close;
            b.vwap = (high+low+close)/3.0; b.volume = vol;
            bars.push_back(b);
            p = close;
        }
        return bars;
    }

    // ── LRU cache ──────────────────────────────────────────────────────────
    void cache_put(CacheKey key, std::shared_ptr<DataFeed> feed) {
        std::lock_guard lk(mu_);
        lru_.remove(key);
        lru_.push_front(key);
        cache_[key] = feed;
        while (cache_size() > cache_cap_) evict_lru();
    }

    std::shared_ptr<DataFeed> cache_get(CacheKey key) {
        std::lock_guard lk(mu_);
        auto it = cache_.find(key);
        if (it == cache_.end()) return nullptr;
        lru_.remove(key);
        lru_.push_front(key);
        return it->second;
    }

    std::size_t cache_size() const {
        std::size_t total = 0;
        for (auto& [k, f] : cache_) total += f->size();
        return total;
    }

    void evict_lru() {
        if (lru_.empty()) return;
        auto oldest = lru_.back(); lru_.pop_back();
        cache_.erase(oldest);
    }

    // ── String helpers ──────────────────────────────────────────────────────
    static std::vector<std::string> split_csv(const std::string& s, char d) {
        std::vector<std::string> r;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, d)) r.push_back(tok);
        return r;
    }

    static std::string trim(std::string s) {
        auto is_ws = [](unsigned char c){ return std::isspace(c); };
        s.erase(s.begin(), std::find_if_not(s.begin(),s.end(),is_ws));
        s.erase(std::find_if_not(s.rbegin(),s.rend(),is_ws).base(), s.end());
        if (s.size()>=2 && s.front()=='"' && s.back()=='"') s=s.substr(1,s.size()-2);
        return s;
    }

    static Timestamp parse_date(const std::string& s) {
        std::tm tm = {};
        std::istringstream ss(s);
        if (s.size() == 10) ss >> std::get_time(&tm, "%Y-%m-%d");
        else if (s.find('T')!=std::string::npos) ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        else ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return 0;
#ifdef _WIN32
        return static_cast<Timestamp>(_mkgmtime(&tm)) * NS_PER_SEC;
#else
        return static_cast<Timestamp>(timegm(&tm)) * NS_PER_SEC;
#endif
    }

    mutable std::mutex mu_;
    std::size_t cache_cap_;
    std::unordered_map<CacheKey, std::shared_ptr<DataFeed>, CacheKeyHash> cache_;
    std::list<CacheKey> lru_;
    std::unordered_map<InstrumentID, std::vector<CorporateAction>> corp_actions_;
};

// ── Global data manager singleton ─────────────────────────────────────────
inline DataManager& data_mgr() {
    static DataManager dm;
    return dm;
}

} // namespace ql