#pragma once

#include <cstdint>
#include <cstring>
#include <array>


struct alignas(8) MarketEvent {
    uint64_t  sequence;          // monotonic sequence written by producer
    uint64_t  producer_ts_ns;    // CLOCK_MONOTONIC_RAW at push time (latency stamp)
    int64_t   bid_price;         // fixed-point, e.g. price * 1e6
    uint64_t  bid_qty;
    int64_t   ask_price;
    uint64_t  ask_qty;
    int64_t   last_price;
    uint64_t  last_qty;
    uint64_t  instrument_id;
    uint32_t  venue_id;
    uint32_t  flags;
    char      symbol[16];        // null-terminated, max 15 chars
    uint8_t   _pad[32];          // pad to 128 bytes total

    // Factory helper used in tests / bench
    static MarketEvent make(uint64_t seq, uint64_t ts_ns,
                            uint64_t iid = 1, const char* sym = "AAPL") noexcept {
        MarketEvent e{};
        e.sequence       = seq;
        e.producer_ts_ns = ts_ns;
        e.instrument_id  = iid;
        e.bid_price      = 150'000'000LL;
        e.bid_qty        = 100;
        e.ask_price      = 150'001'000LL;
        e.ask_qty        = 200;
        e.last_price     = 150'000'500LL;
        e.last_qty       = 50;
        e.venue_id       = 1;
        e.flags          = 0;
        std::strncpy(e.symbol, sym, sizeof(e.symbol) - 1);
        return e;
    }
};

static_assert(sizeof(MarketEvent) == 128,
    "MarketEvent must be exactly 128 bytes (2 cache lines)");
static_assert(alignof(MarketEvent) == 8,
    "MarketEvent alignment must be 8");
