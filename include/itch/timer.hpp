#pragma once
// Low-overhead timestamp counter. x86: rdtscp (serialising on the read side), calibrated against
// steady_clock. aarch64: the generic timer's virtual counter (cntvct_el0) with an isb.
// NOTE: on the Apple Silicon machine this was developed on, cntfrq_el0 reports 1 GHz but the
// counter actually advances in ~41.7 ns steps (measured: deltas of 41/42/83/84/125 ticks), so
// per-message latencies there are quantised to that step. timer_granularity_ns() measures the
// real step so every result row carries it; use batch mode or an x86 host for tail latency.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace itch {

inline uint64_t ticks() {
#if defined(__x86_64__)
    unsigned aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per tick.
inline double ns_per_tick() {
#if defined(__aarch64__)
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return 1e9 / static_cast<double>(f);
#elif defined(__x86_64__)
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const uint64_t c0 = ticks();
    while (clk::now() - t0 < std::chrono::milliseconds(100)) {
    }
    const uint64_t c1 = ticks();
    const auto t1 = clk::now();
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
           static_cast<double>(c1 - c0);
#else
    return 1.0;  // steady_clock ticks are already ns on the platforms we target
#endif
}

// Median non-zero step between distinct counter readings: the effective timer resolution.
inline double timer_granularity_ns(double ns_tick) {
    std::vector<uint64_t> d;
    d.reserve(20001);
    uint64_t prev = ticks();
    while (d.size() < 20001) {
        const uint64_t c = ticks();
        if (c != prev) {
            d.push_back(c - prev);
            prev = c;
        }
    }
    std::nth_element(d.begin(), d.begin() + 10000, d.end());
    return static_cast<double>(d[10000]) * ns_tick;
}

}  // namespace itch
