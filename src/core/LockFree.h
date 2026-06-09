#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/core/LockFree.h
// Lock-free data structures for high-throughput event passing.
//
// SPSCQueue: single-producer single-consumer ring buffer.
//   Used for: market data → strategy, strategy → OMS
//
// MPMCQueue: multi-producer multi-consumer (bounded, cache-padded).
//   Used for: parallel factor computation results → aggregator
//
// Based on Dmitry Vyukov's queue designs with C++20 improvements.
// ═══════════════════════════════════════════════════════════════════════════
#include <atomic>
#include <array>
#include <optional>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace ql {

// ── Cache line size ────────────────────────────────────────────────────────
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t CACHE_LINE = 64;
#endif

// ── SPSC Ring Buffer (single producer, single consumer) ────────────────────
// Capacity must be power of 2. Zero heap allocation after construction.
template<typename T, std::size_t Capacity>
requires (Capacity > 0 && (Capacity & (Capacity-1)) == 0)  // C++20 requires
class SPSCQueue {
    static constexpr std::size_t MASK = Capacity - 1;
public:
    SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer side
    template<typename... Args>
    bool push(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & MASK;
        if (next == read_.load(std::memory_order_acquire)) return false; // full
        new (&slots_[w]) T(std::forward<Args>(args)...);
        write_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side
    std::optional<T> pop() noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return std::nullopt; // empty
        T val = std::move(*reinterpret_cast<T*>(&slots_[r]));
        reinterpret_cast<T*>(&slots_[r])->~T();
        read_.store((r + 1) & MASK, std::memory_order_release);
        return val;
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) ==
               write_.load(std::memory_order_acquire);
    }

    std::size_t size() const noexcept {
        auto w = write_.load(std::memory_order_acquire);
        auto r = read_.load(std::memory_order_acquire);
        return (w - r + Capacity) & MASK;
    }

private:
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    alignas(CACHE_LINE) std::atomic<std::size_t> write_{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> read_{0};
    std::array<Storage, Capacity> slots_;
};

// ── MPMC Bounded Queue ─────────────────────────────────────────────────────
// Suitable for parallel factor computation output collection.
template<typename T, std::size_t Capacity>
requires (Capacity > 0 && (Capacity & (Capacity-1)) == 0)
class MPMCQueue {
    static constexpr std::size_t MASK = Capacity - 1;

    struct Slot {
        alignas(CACHE_LINE) std::atomic<std::size_t> sequence;
        T data;
    };

public:
    MPMCQueue() {
        for (std::size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(T val) noexcept {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & MASK];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq)
                               - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos+1,
                    std::memory_order_relaxed)) {
                    slot.data = std::move(val);
                    slot.sequence.store(pos+1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) return false; // full
            else pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }

    std::optional<T> pop() noexcept {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & MASK];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq)
                               - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos+1,
                    std::memory_order_relaxed)) {
                    T val = std::move(slot.data);
                    slot.sequence.store(pos + Capacity, std::memory_order_release);
                    return val;
                }
            } else if (diff < 0) return std::nullopt; // empty
            else pos = dequeue_pos_.load(std::memory_order_relaxed);
        }
    }

private:
    alignas(CACHE_LINE) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(CACHE_LINE) std::atomic<std::size_t> dequeue_pos_{0};
    std::array<Slot, Capacity> slots_;
};

// ── Intrusive node for wait-free linked structures ─────────────────────────
struct IntrusiveNode {
    std::atomic<IntrusiveNode*> next{nullptr};
};

} // namespace ql
