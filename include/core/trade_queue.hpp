#pragma once

#include "types/trade.hpp"
#include <atomic>
#include <array>
#include <optional>
#include <new>

namespace hft {

// ═══════════════════════════════════════════════════════════════════════════
// Cache line size for alignment
// ═══════════════════════════════════════════════════════════════════════════

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

// ═══════════════════════════════════════════════════════════════════════════
// SPSC Lock-Free Ring Buffer for single strategy -> executor
// ═══════════════════════════════════════════════════════════════════════════

template<size_t Capacity = 65536>
class SPSCTradeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
    
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    alignas(CACHE_LINE_SIZE) std::array<Trade, Capacity> buffer_;

public:
    SPSCTradeQueue() = default;
    
    // Non-copyable, non-movable
    SPSCTradeQueue(const SPSCTradeQueue&) = delete;
    SPSCTradeQueue& operator=(const SPSCTradeQueue&) = delete;
    SPSCTradeQueue(SPSCTradeQueue&&) = delete;
    SPSCTradeQueue& operator=(SPSCTradeQueue&&) = delete;

    bool try_push(const Trade& trade) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }
        
        buffer_[tail] = trade;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    std::optional<Trade> try_pop() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Queue empty
        }
        
        Trade trade = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return trade;
    }

    // Peek without consuming
    std::optional<Trade> peek() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        
        return buffer_[head];
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail - head + Capacity) & MASK;
    }

    size_t capacity() const noexcept {
        return Capacity - 1;  // One slot reserved for full/empty distinction
    }

    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// MPSC Queue - Multiple strategies can push, single executor pops
// Uses a bounded MPMC queue with single consumer optimization
// ═══════════════════════════════════════════════════════════════════════════

template<size_t Capacity = 65536>
class MPSCTradeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    struct Slot {
        std::atomic<uint64_t> sequence;
        Trade data;
        
        Slot() : sequence(0) {}
    };

    alignas(CACHE_LINE_SIZE) std::array<Slot, Capacity> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) size_t dequeue_pos_{0};  // Single consumer, no atomic needed

public:
    MPSCTradeQueue() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Non-copyable, non-movable
    MPSCTradeQueue(const MPSCTradeQueue&) = delete;
    MPSCTradeQueue& operator=(const MPSCTradeQueue&) = delete;
    MPSCTradeQueue(MPSCTradeQueue&&) = delete;
    MPSCTradeQueue& operator=(MPSCTradeQueue&&) = delete;

    // Thread-safe push (multiple producers)
    bool try_push(const Trade& trade) noexcept {
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        
        for (;;) {
            Slot& slot = buffer_[pos & MASK];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, 
                    std::memory_order_relaxed)) {
                    slot.data = trade;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // Queue full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // Single-consumer pop (NOT thread-safe for multiple consumers)
    std::optional<Trade> try_pop() noexcept {
        Slot& slot = buffer_[dequeue_pos_ & MASK];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeue_pos_ + 1);
        
        if (diff < 0) {
            return std::nullopt;  // Queue empty
        }
        
        Trade trade = slot.data;
        slot.sequence.store(dequeue_pos_ + Capacity, std::memory_order_release);
        ++dequeue_pos_;
        return trade;
    }

    // Peek without consuming (single consumer only)
    std::optional<Trade> peek() const noexcept {
        const Slot& slot = buffer_[dequeue_pos_ & MASK];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeue_pos_ + 1);
        
        if (diff < 0) {
            return std::nullopt;
        }
        
        return slot.data;
    }

    bool empty() const noexcept {
        const Slot& slot = buffer_[dequeue_pos_ & MASK];
        size_t seq = slot.sequence.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeue_pos_ + 1) < 0;
    }

    size_t capacity() const noexcept {
        return Capacity;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Priority Trade Queue - Orders sorted by priority then timestamp
// Uses separate queues per priority level for O(1) operations
// ═══════════════════════════════════════════════════════════════════════════

template<size_t CapacityPerLevel = 8192, size_t NumLevels = 4>
class PriorityTradeQueue {
    // Priority levels: 0=highest (market orders), 1=high, 2=normal, 3=low
    std::array<SPSCTradeQueue<CapacityPerLevel>, NumLevels> queues_;

    static size_t priority_to_level(uint8_t priority) {
        if (priority < 32) return 0;       // Highest: market orders, stops
        if (priority < 96) return 1;       // High
        if (priority < 192) return 2;      // Normal
        return 3;                          // Low
    }

public:
    bool try_push(const Trade& trade) noexcept {
        size_t level = priority_to_level(trade.priority);
        return queues_[level].try_push(trade);
    }

    std::optional<Trade> try_pop() noexcept {
        // Always drain highest priority first
        for (size_t i = 0; i < NumLevels; ++i) {
            if (auto trade = queues_[i].try_pop()) {
                return trade;
            }
        }
        return std::nullopt;
    }

    bool empty() const noexcept {
        for (const auto& q : queues_) {
            if (!q.empty()) return false;
        }
        return true;
    }

    size_t size() const noexcept {
        size_t total = 0;
        for (const auto& q : queues_) {
            total += q.size();
        }
        return total;
    }
};

} // namespace hft
