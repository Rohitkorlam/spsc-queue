#include <gtest/gtest.h>

#include "spsc_ring.hpp"
#include "market_event.hpp"
#include "thread_utils.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <numeric>
#include <iostream>
#include <chrono>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr std::size_t kSmallCap  = 8;    // small cap for overflow tests
static constexpr std::size_t kMediumCap = 1024;

static const int kProdCpu = 0;
static const int kConsCpu = []() -> int {
    const int n = available_cpus();
    if (n >= 4) return 2;
    if (n >= 2) return 1;
    return 0;
}();

// Simple integer ring for most correctness tests (less verbose than MarketEvent)
using IntRing = SpscRing<uint64_t, kSmallCap>;
using MidRing = SpscRing<uint64_t, kMediumCap>;
using EventRing = SpscRing<MarketEvent, kMediumCap>;

// ---------------------------------------------------------------------------
// Basic correctness
// ---------------------------------------------------------------------------

TEST(SpscRingBasic, EmptyOnConstruction) {
    IntRing ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.full());
    EXPECT_EQ(ring.size_approx(), 0u);
    EXPECT_EQ(ring.push_count(), 0u);
    EXPECT_EQ(ring.drop_count(), 0u);
}

TEST(SpscRingBasic, PushPopSingleElement) {
    IntRing ring;
    EXPECT_TRUE(ring.try_push(42u));
    EXPECT_FALSE(ring.empty());

    uint64_t out = 0;
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 42u);
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBasic, PopOnEmptyReturnsFalse) {
    IntRing ring;
    uint64_t out = 0;
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRingBasic, PushCountIncrements) {
    IntRing ring;
    for (uint64_t i = 0; i < 3; ++i) (void)ring.try_push(i);
    EXPECT_EQ(ring.push_count(), 3u);
}

TEST(SpscRingBasic, FifoOrder) {
    MidRing ring;
    constexpr uint64_t kN = 100;
    for (uint64_t i = 0; i < kN; ++i) ASSERT_TRUE(ring.try_push(i));
    for (uint64_t i = 0; i < kN; ++i) {
        uint64_t out = 0;
        ASSERT_TRUE(ring.try_pop(out));
        ASSERT_EQ(out, i) << "FIFO order violated at index " << i;
    }
}

// ---------------------------------------------------------------------------
// Overflow / drop observable
// ---------------------------------------------------------------------------

TEST(SpscRingOverflow, FullRingReturnsFalse) {
    IntRing ring;
    // Ring capacity = 8; can hold at most 7 live elements (one slot sacrificed)
    for (std::size_t i = 0; i < kSmallCap - 1; ++i) {
        ASSERT_TRUE(ring.try_push(static_cast<uint64_t>(i)));
    }
    EXPECT_TRUE(ring.full());

    // One more push must fail; caller records the drop explicitly
    bool pushed = ring.try_push(999u);
    EXPECT_FALSE(pushed);
    if (!pushed) ring.record_drop();
    EXPECT_EQ(ring.drop_count(), 1u);
}

TEST(SpscRingOverflow, DropsAreCountedAccurately) {
    IntRing ring;
    // Fill the ring
    for (std::size_t i = 0; i < kSmallCap - 1; ++i) (void)ring.try_push(0u);

    constexpr uint64_t kExtraAttempts = 10;
    for (uint64_t i = 0; i < kExtraAttempts; ++i) {
        if (!ring.try_push(0u)) ring.record_drop();
    }
    EXPECT_EQ(ring.drop_count(), kExtraAttempts);
}

// ---------------------------------------------------------------------------
// Wrap-around (capacity boundary)
// ---------------------------------------------------------------------------

TEST(SpscRingWrap, WrapAroundPreservesOrder) {
    MidRing ring;
    // Push and pop in alternating batches to exercise the index wrap
    uint64_t produced = 0, consumed = 0;
    for (int round = 0; round < 10; ++round) {
        // Push half the capacity
        for (std::size_t i = 0; i < kMediumCap / 2; ++i) {
            ASSERT_TRUE(ring.try_push(produced++));
        }
        // Pop half
        for (std::size_t i = 0; i < kMediumCap / 2; ++i) {
            uint64_t out = 0;
            ASSERT_TRUE(ring.try_pop(out));
            ASSERT_EQ(out, consumed++) << "order broken at consumed=" << consumed;
        }
    }
}

// ---------------------------------------------------------------------------
// MarketEvent payload correctness
// ---------------------------------------------------------------------------

TEST(SpscRingMarketEvent, PayloadIntegrity) {
    EventRing ring;
    constexpr int kEvents = 500;
    for (int i = 0; i < kEvents; ++i) {
        auto ev = MarketEvent::make(static_cast<uint64_t>(i), 0, 42, "ES");
        ASSERT_TRUE(ring.try_push(ev));
    }
    for (int i = 0; i < kEvents; ++i) {
        MarketEvent out{};
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out.sequence, static_cast<uint64_t>(i));
        EXPECT_EQ(out.instrument_id, 42u);
        EXPECT_STREQ(out.symbol, "ES");
    }
}

// ---------------------------------------------------------------------------
// Ordering stress test
//
// This is the "deliberately try to provoke torn read or ordering bug" test.
//
// Strategy:
//   - Producer pushes N events in back-to-back bursts, writing a strict
//     monotonic sequence number into each event.
//   - Consumer verifies:
//       (a) No gaps (sequence is strictly contiguous).
//       (b) No reorders (each sequence is exactly previous + 1).
//       (c) Payload values are consistent (bid_price matches the sequence).
//   - To maximise contention we keep the ring small (16 slots) relative to
//     the event count (200,000) and use no sleeps on either side.
//   - Running under ThreadSanitizer (TSan) provides an additional layer:
//     TSan instruments all atomic operations and will flag any actual
//     data race that this test (or the sanitizer's randomised scheduling)
//     manages to expose.
//
// What a torn read would look like:
//   If the producer's write to slots_[i] were not fully visible before the
//   consumer read it (i.e., missing the release/acquire pair), the consumer
//   could see a partially written MarketEvent — for example, sequence = N
//   but bid_price still holding the previous event's value.  We catch this
//   by encoding the sequence into bid_price and cross-checking.
// ---------------------------------------------------------------------------

TEST(SpscRingOrdering, BurstOrderingStressTest) {
    constexpr std::size_t kRingCap   = 16;   // small ring → maximum contention
    constexpr uint64_t    kTotalEvts = 200'000;

    SpscRing<MarketEvent, kRingCap> ring;

    std::atomic<bool>    consumer_ok{true};
    std::atomic<uint64_t> consumer_errors{0};

    std::thread producer([&] {
        pin_thread(kProdCpu);
        for (uint64_t seq = 0; seq < kTotalEvts; ++seq) {
            MarketEvent ev = MarketEvent::make(seq, 0);
            // Encode sequence into bid_price for cross-check in consumer
            ev.bid_price = static_cast<int64_t>(seq);
            while (!ring.try_push(ev)) {}
        }
    });

    std::thread consumer([&] {
        pin_thread(kConsCpu);
        uint64_t expected_seq = 0;
        while (expected_seq < kTotalEvts) {
            MarketEvent ev{};
            if (!ring.try_pop(ev)) continue;

            // Check sequence is contiguous (no reorder, no gap)
            if (ev.sequence != expected_seq) {
                consumer_ok.store(false, std::memory_order_relaxed);
                consumer_errors.fetch_add(1, std::memory_order_relaxed);
            }

            // Cross-check: bid_price should match sequence (torn read detection)
            if (ev.bid_price != static_cast<int64_t>(ev.sequence)) {
                consumer_ok.store(false, std::memory_order_relaxed);
                consumer_errors.fetch_add(1, std::memory_order_relaxed);
            }

            ++expected_seq;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(consumer_ok.load())
        << "Ordering / torn-read errors detected: "
        << consumer_errors.load();
    EXPECT_EQ(consumer_errors.load(), 0u);
    EXPECT_EQ(ring.drop_count(), 0u)
        << "Unexpected drops in ordering stress test";
}

// ---------------------------------------------------------------------------
// Burst contention: fill/drain cycle at high speed
// ---------------------------------------------------------------------------

TEST(SpscRingStress, HighContention_FillDrainCycles) {
    constexpr std::size_t kRingCap = 32;
    constexpr uint64_t    kCycles  = 5'000;
    constexpr std::size_t kBatch   = kRingCap - 1;  // fill to capacity each cycle

    SpscRing<MarketEvent, kRingCap> ring;

    std::atomic<uint64_t> total_consumed{0};

    std::thread consumer([&] {
        pin_thread(kConsCpu);
        uint64_t consumed = 0;
        while (consumed < kCycles * kBatch) {
            MarketEvent ev{};
            if (ring.try_pop(ev)) ++consumed;
        }
        total_consumed.store(consumed, std::memory_order_relaxed);
    });

    pin_thread(kProdCpu);
    for (uint64_t c = 0; c < kCycles; ++c) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            MarketEvent ev = MarketEvent::make(c * kBatch + i, 0);
            while (!ring.try_push(ev)) {}
        }
    }

    consumer.join();
    EXPECT_EQ(total_consumed.load(), kCycles * kBatch);
    EXPECT_EQ(ring.drop_count(), 0u);
}

// ---------------------------------------------------------------------------
// Compile-time constraints (static assertions are checked at compile time,
// but we also add a runtime test for documentation)
// ---------------------------------------------------------------------------

TEST(SpscRingMeta, CapacityIsReportedCorrectly) {
    EXPECT_EQ((SpscRing<uint64_t, 16>::capacity()), 16u);
    EXPECT_EQ((SpscRing<uint64_t, 1024>::capacity()), 1024u);
}
