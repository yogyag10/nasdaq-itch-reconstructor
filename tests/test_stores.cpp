#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "itch/hdr_histogram.hpp"
#include "itch/order_store.hpp"

using namespace itch;

template <class S>
class StoreTest : public ::testing::Test {};
using Stores = ::testing::Types<StdOrderStore, FlatOrderStore>;
TYPED_TEST_SUITE(StoreTest, Stores);

TYPED_TEST(StoreTest, BasicInsertFindErase) {
    TypeParam s(4);
    EXPECT_TRUE(s.insert(5, OrderRec{100, 10, 1, Side::Buy}));
    EXPECT_FALSE(s.insert(5, OrderRec{200, 20, 1, Side::Buy}));  // duplicate rejected, unchanged
    ASSERT_NE(s.find(5), nullptr);
    EXPECT_EQ(s.find(5)->price, 100u);
    EXPECT_EQ(s.find(6), nullptr);
    EXPECT_TRUE(s.erase(5));
    EXPECT_FALSE(s.erase(5));
    EXPECT_EQ(s.find(5), nullptr);
    EXPECT_EQ(s.size(), 0u);
}

// Randomised model check against std::unordered_map: exercises growth and, for the flat map,
// collision chains and backward-shift deletion (small key range => many collisions).
TYPED_TEST(StoreTest, MatchesModelUnderRandomChurn) {
    TypeParam s(2);
    std::unordered_map<uint64_t, uint32_t> model;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200000; ++i) {
        const uint64_t k = rng() % 3000;
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            const uint32_t v = static_cast<uint32_t>(rng());
            const bool ins = s.insert(k, OrderRec{v, v, 1, Side::Sell});
            EXPECT_EQ(ins, model.emplace(k, v).second);
        } else if (op == 1) {
            EXPECT_EQ(s.erase(k), model.erase(k) != 0);
        } else {
            auto* r = s.find(k);
            auto it = model.find(k);
            ASSERT_EQ(r != nullptr, it != model.end());
            if (r) EXPECT_EQ(r->price, it->second);
        }
        ASSERT_EQ(s.size(), model.size());
    }
    size_t seen = 0;
    s.for_each([&](uint64_t k, const OrderRec& r) {
        ASSERT_TRUE(model.count(k));
        EXPECT_EQ(model[k], r.price);
        ++seen;
    });
    EXPECT_EQ(seen, model.size());
}

TYPED_TEST(StoreTest, SequentialRefsLikeRealFeed) {
    TypeParam s(16);
    for (uint64_t k = 1000; k < 101000; ++k) ASSERT_TRUE(s.insert(k, OrderRec{1, 1, 1, Side::Buy}));
    for (uint64_t k = 1000; k < 101000; k += 2) ASSERT_TRUE(s.erase(k));
    for (uint64_t k = 1000; k < 101000; ++k) EXPECT_EQ(s.find(k) != nullptr, (k & 1) != 0);
}

TEST(Hdr, RelativeErrorBoundAndPercentiles) {
    HdrHistogram h;
    for (uint64_t v = 1; v <= 100000; ++v) h.record(v);
    EXPECT_EQ(h.total(), 100000u);
    auto near = [](uint64_t got, double want) {
        const double g = static_cast<double>(got);
        return g >= want * 0.99 && g <= want * 1.0101 + 1;
    };
    EXPECT_TRUE(near(h.percentile(0.50), 50000)) << h.percentile(0.50);
    EXPECT_TRUE(near(h.percentile(0.99), 99000)) << h.percentile(0.99);
    EXPECT_TRUE(near(h.percentile(0.999), 99900)) << h.percentile(0.999);
    HdrHistogram g;
    g.record(5);
    g.record(1ull << 30);
    EXPECT_EQ(g.percentile(0.5), 5u);  // exact for small values
    EXPECT_GE(g.percentile(1.0), 1ull << 30);
    HdrHistogram m = h;
    m.merge(g);
    EXPECT_EQ(m.total(), 100002u);
}
