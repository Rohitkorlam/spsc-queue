#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <optional>
#include <stdexcept>


template <typename T, std::size_t N>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
        "SpscRing requires a trivially copyable element type");
    static_assert(N >= 2 && (N & (N - 1)) == 0,
        "SpscRing capacity N must be a power of two >= 2");

public:
    SpscRing() noexcept
        : tail_(0), head_(0), push_count_(0), drop_count_(0) {}

    // Non-copyable, non-movable — the ring owns pinned storage
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    //bool result_of_push = ring.try_push(1);
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & kMask;

        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }

        slots_[tail] = item;  // write before making the slot visible
        tail_.store(next, std::memory_order_release);
        push_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void record_drop() noexcept {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[head];  // read after tail acquire

        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return (t - h + N) & kMask;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::size_t next =
            (tail_.load(std::memory_order_acquire) + 1) & kMask;
        return next == head_.load(std::memory_order_acquire);
    }

    /// Total successful pushes since construction.
    [[nodiscard]] uint64_t push_count() const noexcept {
        return push_count_.load(std::memory_order_relaxed);
    }

    /// Total events dropped because the ring was full.
    [[nodiscard]] uint64_t drop_count() const noexcept {
        return drop_count_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept { return N; }

private:
    static constexpr std::size_t kMask = N - 1;
    static constexpr std::size_t kCacheLineSize = 64;

    alignas(kCacheLineSize) T slots_[N];

    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;
    // Pad to the next cache line so push_count_ doesn't share with tail_.
    char _pad0[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    // Consumer-owned: head is written only by the consumer.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    char _pad1[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    alignas(kCacheLineSize) std::atomic<uint64_t> push_count_;
    std::atomic<uint64_t> drop_count_;
};
