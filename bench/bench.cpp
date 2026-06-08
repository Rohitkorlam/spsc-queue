#include <benchmark/benchmark.h>

#include "spsc_ring.hpp"
#include "market_event.hpp"
#include "latency.hpp"
#include "thread_utils.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <barrier>

static const int kProducerCpu = 0;
static const int kConsumerCpu = []() -> int {
    const int n = available_cpus();
    if (n >= 4) return 2;
    if (n >= 2) return 1;
    return 0;
}();
static constexpr std::size_t kWarmupEvents = 1'000;


template <std::size_t N>
static void BM_Throughput(benchmark::State& state) {
    auto ring = std::make_unique<SpscRing<MarketEvent, N>>();
    
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> consumed{0};

    std::thread consumer([&] {
        pin_thread(kConsumerCpu);
        MarketEvent ev;
        while (!stop.load(std::memory_order_relaxed)) {
            if (ring->try_pop(ev)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        while (ring->try_pop(ev)) {
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    pin_thread(kProducerCpu);

    uint64_t seq = 0;
    for (auto _ : state) {
        MarketEvent ev = MarketEvent::make(seq++, 0);
        while (!ring->try_push(ev)) {}
    }

    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.counters["drops"]    = static_cast<double>(ring->drop_count());
    state.counters["consumed"] = static_cast<double>(consumed.load());
}

BENCHMARK(BM_Throughput<1024>)   ->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Throughput<16384>)  ->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Throughput<262144>) ->Unit(benchmark::kNanosecond)->UseRealTime();

// ---------------------------------------------------------------------------
// BM_Latency — per-event timestamp, full distribution reported
// ---------------------------------------------------------------------------

template <std::size_t N>
static void BM_Latency(benchmark::State& state) {
    static constexpr std::size_t kSampleCount = 500'000;

    auto ring = std::make_unique<SpscRing<MarketEvent, N>>();
    LatencyHistogram hist;
    hist.reserve(kSampleCount + kWarmupEvents);

    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        pin_thread(kConsumerCpu);
        MarketEvent ev;
        std::size_t pops = 0;
        while (pops < kSampleCount) {
            if (ring->try_pop(ev)) {
                const uint64_t now = now_ns();
                if (pops >= kWarmupEvents) {
                    hist.record(now - ev.producer_ts_ns);
                }
                ++pops;
            }
        }
        producer_done.store(true, std::memory_order_relaxed);
    });

    pin_thread(kProducerCpu);

    for (auto _ : state) {
        uint64_t seq = 0;
        while (seq < kSampleCount) {
            MarketEvent ev = MarketEvent::make(++seq, now_ns());
            while (!ring->try_push(ev)) {}
        }
        while (!producer_done.load(std::memory_order_relaxed)) { /* spin */ }
        producer_done.store(false, std::memory_order_relaxed);
    }

    consumer.join();

    hist.sort();

    // Report percentiles as custom counters (visible in benchmark output)
    state.counters["p50_ns"]   = static_cast<double>(hist.percentile(50.0));
    state.counters["p99_ns"]   = static_cast<double>(hist.percentile(99.0));
    state.counters["p99_9_ns"] = static_cast<double>(hist.percentile(99.9));
    state.counters["mean_ns"]  = hist.mean();
    state.counters["min_ns"]   = static_cast<double>(hist.min_ns());
    state.counters["max_ns"]   = static_cast<double>(hist.max_ns());
    state.counters["drops"]    = static_cast<double>(ring->drop_count());
    state.SetItemsProcessed(static_cast<int64_t>(kSampleCount));

    // Print ASCII histogram to stdout (visible after benchmark output)
    // std::printf("\n--- Latency histogram  capacity=%zu ---\n", N);
    // std::printf("%s", hist.ascii_histogram(20).c_str());
}

BENCHMARK(BM_Latency<1024>)
    ->Unit(benchmark::kNanosecond)->UseRealTime()->Iterations(1);
BENCHMARK(BM_Latency<16384>)
    ->Unit(benchmark::kNanosecond)->UseRealTime()->Iterations(1);
BENCHMARK(BM_Latency<262144>)
   ->Unit(benchmark::kNanosecond)->UseRealTime()->Iterations(1);

BENCHMARK_MAIN();
