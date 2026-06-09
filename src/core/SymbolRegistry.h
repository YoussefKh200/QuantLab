#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/core/SymbolRegistry.h
// Central mapping: ticker string ↔ InstrumentID (compact 64-bit hash)
// AssetMetadata: static per-instrument properties (tick size, lot size, etc.)
// ═══════════════════════════════════════════════════════════════════════════
#include "Types.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <stdexcept>
#include <mutex>
#include <functional>

namespace ql {

// ── Asset metadata ─────────────────────────────────────────────────────────
struct AssetMetadata {
    InstrumentID id            = 0;
    std::string  ticker;           // primary ticker (e.g. "AAPL")
    std::string  name;             // full name
    std::string  isin;             // ISIN
    std::string  cusip;
    AssetClass   asset_class   = AssetClass::Equity;
    Exchange     primary_exchange = Exchange::NASDAQ;
    GICSSector   sector        = GICSSector::Unknown;
    std::string  currency      = "USD";

    // Microstructure parameters
    double       tick_size     = 0.01;     // minimum price increment
    double       lot_size      = 1.0;      // minimum order size (shares)
    double       multiplier    = 1.0;      // contract multiplier (futures)
    double       margin_rate   = 0.25;     // initial margin requirement

    // Corporate action tracking
    double       split_factor  = 1.0;      // cumulative split adjustment
    double       div_factor    = 1.0;      // cumulative dividend adjustment

    // Listing dates
    Timestamp    ipo_date      = 0;
    Timestamp    delist_date   = 0;        // 0 = still listed

    bool is_listed(Timestamp ts) const noexcept {
        if (ipo_date > 0 && ts < ipo_date)      return false;
        if (delist_date > 0 && ts >= delist_date) return false;
        return true;
    }
};

// ── FNV-1a hash for ticker strings ────────────────────────────────────────
inline InstrumentID hash_ticker(std::string_view ticker) noexcept {
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    std::uint64_t h = FNV_OFFSET;
    for (unsigned char c : ticker) {
        h ^= c;
        h *= FNV_PRIME;
    }
    return h;
}

// ── Symbol registry (thread-safe, singleton) ──────────────────────────────
class SymbolRegistry {
public:
    static SymbolRegistry& instance() {
        static SymbolRegistry reg;
        return reg;
    }

    // Register an instrument; returns its ID
    InstrumentID register_instrument(AssetMetadata meta) {
        if (meta.id == 0) meta.id = hash_ticker(meta.ticker);
        std::lock_guard lk(mu_);
        by_id_[meta.id]     = meta;
        by_ticker_[meta.ticker] = meta.id;
        return meta.id;
    }

    // Lookup by ticker string
    std::optional<InstrumentID> find_id(std::string_view ticker) const {
        std::lock_guard lk(mu_);
        auto it = by_ticker_.find(std::string(ticker));
        if (it == by_ticker_.end()) return std::nullopt;
        return it->second;
    }

    // Get-or-create: auto-register unknown tickers (for convenience in research)
    InstrumentID get_or_create(std::string_view ticker,
                                AssetClass cls = AssetClass::Equity) {
        {
            std::lock_guard lk(mu_);
            auto it = by_ticker_.find(std::string(ticker));
            if (it != by_ticker_.end()) return it->second;
        }
        AssetMetadata m;
        m.ticker      = std::string(ticker);
        m.asset_class = cls;
        return register_instrument(std::move(m));
    }

    const AssetMetadata* get_metadata(InstrumentID id) const {
        std::lock_guard lk(mu_);
        auto it = by_id_.find(id);
        return (it != by_id_.end()) ? &it->second : nullptr;
    }

    const AssetMetadata* get_metadata(std::string_view ticker) const {
        auto id_opt = find_id(ticker);
        if (!id_opt) return nullptr;
        return get_metadata(*id_opt);
    }

    std::string ticker_of(InstrumentID id) const {
        std::lock_guard lk(mu_);
        auto it = by_id_.find(id);
        return (it != by_id_.end()) ? it->second.ticker : "<unknown>";
    }

    std::vector<InstrumentID> all_ids() const {
        std::lock_guard lk(mu_);
        std::vector<InstrumentID> ids;
        ids.reserve(by_id_.size());
        for (auto& [id, _] : by_id_) ids.push_back(id);
        return ids;
    }

    std::size_t size() const {
        std::lock_guard lk(mu_);
        return by_id_.size();
    }

    void clear() {
        std::lock_guard lk(mu_);
        by_id_.clear();
        by_ticker_.clear();
    }

    // Apply corporate action adjustment
    void apply_split(InstrumentID id, double factor) {
        std::lock_guard lk(mu_);
        auto it = by_id_.find(id);
        if (it != by_id_.end())
            it->second.split_factor *= factor;
    }

private:
    SymbolRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<InstrumentID, AssetMetadata> by_id_;
    std::unordered_map<std::string, InstrumentID>   by_ticker_;
};

// ── Convenience free functions ─────────────────────────────────────────────
inline InstrumentID instrument(std::string_view ticker) {
    return SymbolRegistry::instance().get_or_create(ticker);
}

inline std::string ticker(InstrumentID id) {
    return SymbolRegistry::instance().ticker_of(id);
}

} // namespace ql
