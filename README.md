SPSC RING BUFFER

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

./build/spsc_tests

./build/spsc_bench


Requirements: CMake ≥ 3.20, GCC or Clang with C++20 support, pthreads. Google Benchmark and GTest are fetched automatically via FetchContent.


Design

### Payload: `MarketEvent` (128 bytes)

A realistic L1 market-data event: sequence number, nanosecond producer timestamp, bid/ask price+qty (fixed-point), last trade, instrument ID, venue, flags, and an inline 15-char symbol. The struct comes to ~96 bytes of live data, padded to 128 bytes (exactly 2 cache lines).

Ring Buffer: SpscRing<T, N>

- **Fixed capacity at construction**: `N` is a compile-time power-of-two; indices are masked with `N-1` instead of using `%`, which compiles to a single `AND`.
- **One slot sacrificed**: the ring holds at most `N-1` elements at a time to distinguish full from empty without a separate size atomic (which would require extra synchronisation on both sides).
- **API surface**: `try_push(const T&) → bool`, `try_pop(T&) → bool`, `record_drop()`, `push_count()`, `drop_count()`, `size_approx()`, `empty()`, `full()`, `capacity()`.
- **Overflow is never silent**: `try_push` returns `false` when full. The caller chooses to retry (spin) or discard and call `record_drop()`. Spinning does not increment the drop counter; only an explicit `record_drop()` call does. This separates two distinct application policies — spin-until-delivered vs. best-effort-drop — rather than silently conflating them.

---

## Memory ordering

### Per-operation justification

#### `try_push`

| 1 | `tail_.load` | `relaxed` | The producer is the sole writer of `tail_`. No other thread races on this load; relaxed is sufficient to read our own last-written value. |
| 2 | `head_.load` | `acquire` | We need to see the consumer's most recent `head_` advance before we decide the slot at `slots_[tail]` is free to overwrite. The acquire pairs with the consumer's release store on `head_` in `try_pop`. Without it, we could read a stale `head_`, believe the ring is full, and refuse to push a slot that the consumer has already freed — or worse, on weakly-ordered hardware, overwrite a slot the consumer hasn't read yet. |
| 3 | `slots_[tail] = item` | plain store | Safe: we only reach this line after confirming (via the acquire on head) that this slot is free. No other thread is accessing it. |
| 4 | `tail_.store(next)` | `release` | The release pairs with the consumer's acquire load of `tail_` in `try_pop`. This makes the slot write visible to the consumer before it sees the updated tail. Without the release, the consumer could observe the new `tail_` value but see stale (partially written) slot data — a torn read. |
| 5 | `push_count_.fetch_add` | `relaxed` | Observability counter only; never used to guard slot access. |

#### `try_pop`

| Step | Operation | Ordering | Reasoning |
|---|---|---|---|
| 1 | `head_.load` | `relaxed` | Consumer is the sole writer of `head_`. Same reasoning as tail in try_push. |
| 2 | `tail_.load` | `acquire` | Pairs with the producer's release store of `tail_` in `try_push`. Guarantees the slot write that happened before the producer's tail advance is visible to us before we read the slot. This is the critical ordering that prevents torn reads. |
| 3 | `out = slots_[head]` | plain load | Safe after the acquire on `tail_`. The happens-before chain is: producer writes slot → producer release-stores `tail_` → consumer acquire-loads `tail_` → consumer reads slot. |
| 4 | `head_.store(next)` | `release` | Pairs with the producer's acquire load of `head_` in `try_push`. Signals that the slot is free to overwrite. Without the release, the producer could observe the new `head_` value and start writing the slot before our read is complete. |

### Happens-before chain (full proof)

```
Producer:
  slots_[tail] = item          (A)
  tail_.store(next, release)   (B)   // A happens-before B (sequenced-before)

Consumer:
  tail_.load(acquire) == next  (C)   // C synchronises-with B (acquire/release pair)
  out = slots_[head]           (D)   // C happens-before D (sequenced-before)

Therefore: A happens-before D.  The slot write is always visible before the slot read.

Symmetric chain for head_ ensuring the producer never overwrites an unread slot:

Consumer:
  out = slots_[head]           (E)
  head_.store(next, release)   (F)   // E happens-before F

Producer:
  head_.load(acquire) != next  (G)   // G synchronises-with F
  slots_[tail] = item          (H)   // G happens-before H

Therefore: E happens-before H.  The consumer's read completes before the producer's next write to the same slot.
```

---

## False sharing mitigation

Three hot regions, each on its own 64-byte cache line:

```cpp
alignas(64) T slots_[N];                    // slot array: 64-byte aligned start

alignas(64) std::atomic<std::size_t> tail_; // producer-owned
char _pad0[64 - sizeof(std::atomic<std::size_t>)]; // pad to full line

alignas(64) std::atomic<std::size_t> head_; // consumer-owned
char _pad1[64 - sizeof(std::atomic<std::size_t>)]; // pad to full line

alignas(64) std::atomic<uint64_t> push_count_;
std::atomic<uint64_t> drop_count_;
```

Without this separation, `tail_` and `head_` would land on the same cache line. Every `try_push` writes `tail_` and reads `head_`; every `try_pop` writes `head_` and reads `tail_`. These writes would invalidate the shared line on both cores continuously — a classic false-sharing ping-pong. With the `alignas(64)` separation and explicit padding, each core owns its write target and only reads the other side's line (which changes less frequently in a steady-state flow).

The slot array is also 64-byte aligned so that `slots_[0]` starts at a cache-line boundary. Since `MarketEvent` is 128 bytes (2 cache lines), each slot occupies exactly 2 full lines with no cross-boundary straddling.

---

## Busy-polling vs. sleeping reader

**Choice: busy-poll (spin loop with `spin_wait()`).**

`try_push` and `try_pop` return immediately on full/empty. The caller spins:

**Why busy-poll for this use case:**
- Latency is the primary metric. A futex/condvar sleep adds at minimum 10–30 µs of wake-up latency (kernel scheduling round-trip). A busy-spinning consumer can observe a new tail value within nanoseconds of the release store.
- HFT producers generate events at high enough rate that the consumer is rarely idle for more than a few hundred nanoseconds. Sleeping would add more overhead than it saves.

---

## Latency measurement

**Clock source: `CLOCK_MONOTONIC_RAW`**

The producer stamps `producer_ts_ns = clock_gettime(CLOCK_MONOTONIC_RAW)` into the event immediately before `try_push`. The consumer reads `now_ns()` immediately after `try_pop` returns and records `now - ev.producer_ts_ns`.

`CLOCK_MONOTONIC_RAW` is preferred over `rdtsc` for this benchmark because:
- It is not subject to NTP or `adjtime` corrections — a 10 µs NTP step would appear as an artificial spike in the latency histogram.
- It gives consistent results across cores without requiring TSC synchronisation (which is hardware-dependent and not guaranteed across sockets or in VMs).
- The overhead (~20 ns per call) is small relative to the latencies being measured (typically 100–1000 ns core-to-core).

`rdtsc` would be the right choice in a production path where the timestamp is used for sequencing rather than just measurement, and where invariant TSC is confirmed (`/proc/cpuinfo` flag `constant_tsc`). In that case, a calibration step (ticks → ns conversion) should be run at startup.

**Warmup discipline**

The latency benchmark discards the first `kWarmupEvents = 1000` samples. These suffer from:
- Cold instruction cache (first branch predictions, BTB misses).
- Cold data cache for the slot array (especially large rings that don't fit in L1).
- OS scheduler settling (both threads getting time-sliced before they stabilise).

Google Benchmark also runs the entire benchmark body multiple times and reports the best result, providing an additional outer warmup layer.


## Bonus 1: ARM64 (Graviton)

**What changes on ARM64 vs x86-64:**

x86-64 implements the TSO (Total Store Order) memory model: stores are never reordered with prior stores, and loads are never reordered with prior loads. The only reordering TSO allows is a store-then-load reorder (a StoreLoad). In practice, this means that on x86, `std::memory_order_release` and `std::memory_order_acquire` on `std::atomic` compile to ordinary MOV instructions — the hardware enforces the ordering for free.

ARM64 uses a much weaker memory model (similar to the C++ memory model baseline): stores can be reordered with other stores, and loads can be reordered with other loads. An acquire load compiles to `LDAR` (load-acquire) and a release store to `STLR` (store-release), both of which include a one-way barrier. These are significantly more expensive than plain `LDR`/`STR`.

**Where this matters more on ARM64:**

The critical pairs in our ring buffer — the release store of `tail_` in `try_push` and the acquire load of `tail_` in `try_pop` — are where ARM64 differs most from x86. On x86, these are just MOVs; on ARM64, `STLR`/`LDAR` have higher latency and lower throughput. The round-trip cost (release store on core A → acquire load on core B) will be higher.

The `relaxed` loads (`tail_.load(relaxed)` in `try_push`, `head_.load(relaxed)` in `try_pop`) also behave differently: on x86 a relaxed load is just `MOV` anyway; on ARM64 it is `LDR` without a barrier, which is fine as long as the ordering requirement is met by the subsequent acquire. Our ordering is correct on both architectures — but on ARM64 you should verify with TSan (same invocation, different hardware) and also check that the compiler has not hoisted a relaxed load above an acquire.
