#pragma once
// HDR-style histogram: log-linear buckets. Values are grouped by power of two (the
// "exponent"), and each power-of-two range is split into kSub linear sub-buckets, so the
// relative error of any recorded value is at most 1/kSub (< 1% for kSub = 128) across the whole
// range, in fixed memory, with an O(1) record (one clz + shift + increment).
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace itch {

class HdrHistogram {
  public:
    static constexpr unsigned kSubBits = 7;
    static constexpr uint64_t kSub = 1u << kSubBits;  // 128
    static constexpr unsigned kMaxExp = 40;           // up to ~2^47 ns: far beyond any latency

    HdrHistogram() : counts_((kMaxExp + 1) * kSub, 0) {}

    void record(uint64_t v) {
        ++counts_[index(v)];
        ++total_;
    }
    uint64_t total() const { return total_; }

    // Smallest value v such that at least q of the samples are <= v (upper edge of the bucket).
    uint64_t percentile(double q) const {
        if (total_ == 0) return 0;
        const uint64_t target =
            std::max<uint64_t>(1, static_cast<uint64_t>(q * static_cast<double>(total_) + 0.5));
        uint64_t seen = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target) return upper_edge(i);
        }
        return upper_edge(counts_.size() - 1);
    }
    void merge(const HdrHistogram& o) {
        for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
    }
    // (upper edge, count) for non-empty buckets, for plotting.
    template <class F>
    void for_each_bucket(F&& f) const {
        for (size_t i = 0; i < counts_.size(); ++i)
            if (counts_[i]) f(upper_edge(i), counts_[i]);
    }

  private:
    static size_t index(uint64_t v) {
        if (v < kSub) return static_cast<size_t>(v);  // exact below kSub
        const unsigned exp = static_cast<unsigned>(std::bit_width(v)) - 1 - kSubBits + 1;
        const uint64_t sub = (v >> (exp - 1)) & (kSub - 1);
        const size_t i = static_cast<size_t>(exp) * kSub + static_cast<size_t>(sub);
        return std::min(i, static_cast<size_t>((kMaxExp + 1) * kSub - 1));
    }
    static uint64_t upper_edge(size_t i) {
        const uint64_t exp = i / kSub, sub = i % kSub;
        if (exp == 0) return sub;
        return ((kSub + sub + 1) << (exp - 1)) - 1;
    }
    std::vector<uint64_t> counts_;
    uint64_t total_ = 0;
};

}  // namespace itch
