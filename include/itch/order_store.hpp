#pragma once
// OrderStore: order reference -> live order. Only live orders are stored; deletes free entries.
// Two interchangeable implementations behind the same interface:
//   StdOrderStore  - std::unordered_map baseline (node-based: one heap allocation per order,
//                    pointer chase per lookup).
//   FlatOrderStore - open addressing, linear probing, backward-shift deletion (no tombstones),
//                    one contiguous array so a lookup is usually a single cache line.
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

// FlatOrderStore uses this value to mark an empty slot, so it can never be a stored key. Real
// feeds never get near 2^64-1; BookBuilder rejects it as malformed for every store so the
// designs behave identically on bad input.
inline constexpr OrderRef kReservedRef = ~OrderRef{0};

struct OrderRec {
    Price price;
    uint32_t shares;  // remaining displayed shares, always > 0 while stored
    Locate locate;
    Side side;
};

class StdOrderStore {
  public:
    explicit StdOrderStore(size_t reserve = 1u << 16) { map_.reserve(reserve); }
    bool insert(OrderRef ref, const OrderRec& r) { return map_.emplace(ref, r).second; }
    OrderRec* find(OrderRef ref) {
        auto it = map_.find(ref);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool erase(OrderRef ref) { return map_.erase(ref) != 0; }
    size_t size() const { return map_.size(); }
    template <class F>
    void for_each(F&& f) const {
        for (const auto& [k, v] : map_) f(k, v);
    }

  private:
    std::unordered_map<OrderRef, OrderRec> map_;
};

class FlatOrderStore {
  public:
    explicit FlatOrderStore(size_t reserve = 1u << 16) {
        size_t cap = 16;
        while (cap < reserve * 2) cap <<= 1;
        init(cap);
    }
    bool insert(OrderRef ref, const OrderRec& r) {
        if (ref == kEmpty) return false;
        if ((size_ + 1) * 10 > slots_.size() * 7) grow();  // keep load factor <= 0.7
        size_t i = home(ref);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].key == kEmpty) break;
            if (slots_[i].key == ref) return false;
        }
        slots_[i] = Slot{ref, r};
        ++size_;
        return true;
    }
    OrderRec* find(OrderRef ref) {
        if (ref == kEmpty) return nullptr;
        for (size_t i = home(ref);; i = (i + 1) & mask_) {
            if (slots_[i].key == ref) return &slots_[i].rec;
            if (slots_[i].key == kEmpty) return nullptr;
        }
    }
    bool erase(OrderRef ref) {
        if (ref == kEmpty) return false;
        size_t i = home(ref);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].key == ref) break;
            if (slots_[i].key == kEmpty) return false;
        }
        // Backward-shift: pull later entries of the probe chain into the hole when doing so
        // does not move them before their home slot.
        size_t hole = i;
        for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (slots_[j].key == kEmpty) break;
            const size_t h = home(slots_[j].key);
            // distance from home to j must be >= distance from hole to j to move it
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
        return true;
    }
    size_t size() const { return size_; }
    template <class F>
    void for_each(F&& f) const {
        for (const auto& s : slots_)
            if (s.key != kEmpty) f(s.key, s.rec);
    }

  private:
    static constexpr OrderRef kEmpty = kReservedRef;
    struct Slot {
        OrderRef key;
        OrderRec rec;
    };
    void init(size_t cap) {
        slots_.assign(cap, Slot{kEmpty, OrderRec{}});
        mask_ = cap - 1;
        shift_ = 64 - static_cast<unsigned>(__builtin_ctzll(cap));
        size_ = 0;
    }
    // Fibonacci hashing: order refs are near-sequential, so a multiplicative hash spreads
    // them while keeping the home slot computation to one multiply + shift.
    size_t home(OrderRef k) const {
        return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    void grow() {
        std::vector<Slot> old = std::move(slots_);
        init(old.size() * 2);
        for (const auto& s : old)
            if (s.key != kEmpty) insert(s.key, s.rec);
    }
    std::vector<Slot> slots_;
    size_t mask_ = 0;
    unsigned shift_ = 0;
    size_t size_ = 0;
};

}  // namespace itch
