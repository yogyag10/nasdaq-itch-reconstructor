// Micro-benchmarks for the OrderStore and price-level structures in isolation.
// benchmark::DoNotOptimize / ClobberMemory keep results observable so the compiler cannot
// delete the work; operands come from a pre-generated random stream so RNG cost is outside
// the timed loop.
#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "itch/book.hpp"
#include "itch/order_store.hpp"

using namespace itch;

// Steady-state feed pattern: N live orders; each iteration retires the oldest and adds a new one
// (sequential refs, like Nasdaq's day-unique reference numbers), then looks one up at random.
template <class Store>
static void BM_StoreChurn(benchmark::State& st) {
    const uint64_t live = static_cast<uint64_t>(st.range(0));
    Store s(live);
    for (uint64_t r = 0; r < live; ++r) s.insert(r + 1, OrderRec{100, 100, 1, Side::Buy});
    std::mt19937_64 rng(1);
    std::vector<uint64_t> probes(1u << 16);
    uint64_t next = live + 1, oldest = 1;
    for (auto _ : st) {
        s.erase(oldest++);
        s.insert(next++, OrderRec{100, 100, 1, Side::Buy});
        // probe an order that is live right now
        const uint64_t p = oldest + (rng() % (live - 1));
        OrderRec* r = s.find(p);
        benchmark::DoNotOptimize(r);
    }
    benchmark::ClobberMemory();
    st.SetItemsProcessed(st.iterations());
}
BENCHMARK_TEMPLATE(BM_StoreChurn, StdOrderStore)->Arg(1 << 12)->Arg(1 << 16)->Arg(1 << 20);
BENCHMARK_TEMPLATE(BM_StoreChurn, FlatOrderStore)->Arg(1 << 12)->Arg(1 << 16)->Arg(1 << 20);

template <class Store>
static void BM_StoreFindHit(benchmark::State& st) {
    const uint64_t live = static_cast<uint64_t>(st.range(0));
    Store s(live);
    for (uint64_t r = 0; r < live; ++r) s.insert(r + 1, OrderRec{100, 100, 1, Side::Buy});
    std::mt19937_64 rng(2);
    std::vector<uint64_t> keys(1u << 16);
    for (auto& k : keys) k = 1 + rng() % live;
    size_t i = 0;
    for (auto _ : st) {
        OrderRec* r = s.find(keys[i++ & 0xffff]);
        benchmark::DoNotOptimize(r);
    }
    st.SetItemsProcessed(st.iterations());
}
BENCHMARK_TEMPLATE(BM_StoreFindHit, StdOrderStore)->Arg(1 << 12)->Arg(1 << 16)->Arg(1 << 20);
BENCHMARK_TEMPLATE(BM_StoreFindHit, FlatOrderStore)->Arg(1 << 12)->Arg(1 << 16)->Arg(1 << 20);

// A book side with `depth` populated levels. Each iteration is one add near the top of book
// (offset skewed toward the BBO like real flow) followed by one reduce that may delete a level.
template <class Side>
static void BM_SideAddReduce(benchmark::State& st) {
    const uint32_t depth = static_cast<uint32_t>(st.range(0));
    Side side;
    const Price base = 1000000;
    auto px = [&](uint32_t off) { return Side::kIsBid ? base - off * 100 : base + off * 100; };
    for (uint32_t d = 0; d < depth; ++d) side.add(px(d), 100);
    std::mt19937_64 rng(3);
    std::vector<uint32_t> offs(1u << 16);
    for (auto& o : offs) {
        const uint32_t a = static_cast<uint32_t>(rng() % depth),
                       b = static_cast<uint32_t>(rng() % depth);
        o = a < b ? a : b;  // min of two uniforms: skewed toward the top of book
    }
    size_t i = 0;
    for (auto _ : st) {
        const uint32_t o = offs[i++ & 0xffff];
        side.add(px(o), 100);
        bool ok = side.reduce(px(o), 100, true);
        benchmark::DoNotOptimize(ok);
        Price bp;
        Level lv;
        benchmark::DoNotOptimize(side.best(bp, lv));
    }
    st.SetItemsProcessed(st.iterations());
}
BENCHMARK_TEMPLATE(BM_SideAddReduce, SideMap<true>)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK_TEMPLATE(BM_SideAddReduce, SideVec<true>)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

// Level churn that creates and deletes levels at the top (the expensive case for a flat vector).
template <class Side>
static void BM_SideTopChurn(benchmark::State& st) {
    const uint32_t depth = static_cast<uint32_t>(st.range(0));
    Side side;
    const Price base = 1000000;
    auto px = [&](uint32_t off) { return Side::kIsBid ? base - off * 100 : base + off * 100; };
    for (uint32_t d = 1; d < depth; ++d) side.add(px(d), 100);
    for (auto _ : st) {
        side.add(px(0), 100);  // new best
        Price bp;
        Level lv;
        benchmark::DoNotOptimize(side.best(bp, lv));
        side.reduce(px(0), 100, true);  // best level disappears
    }
    st.SetItemsProcessed(st.iterations());
}
BENCHMARK_TEMPLATE(BM_SideTopChurn, SideMap<true>)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK_TEMPLATE(BM_SideTopChurn, SideVec<true>)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

BENCHMARK_MAIN();
