#pragma once
// Per-symbol book: two sides of aggregate price levels (L2). We deliberately do NOT track
// per-order FIFO queues (L3): the outputs are BBO, depth snapshots and trades, none of which
// need queue position, and L2 keeps a level to 12 bytes. Price-time priority is Nasdaq's
// concern; a replace simply removes and re-adds, which is what loses priority upstream.
//
// Two interchangeable side implementations (best-first iteration, O(1) best):
//   SideMap<IsBid> - std::map baseline, best = begin().
//   SideVec<IsBid> - sorted flat vector ordered worst..best so the best level is back().
//                    Insert/erase near the top of book (where nearly all activity is) moves
//                    only a handful of elements and stays in cache.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

struct Level {
    uint64_t shares = 0;
    uint32_t orders = 0;
};

struct Bbo {
    Price bid_px = 0;
    uint64_t bid_sz = 0;
    Price ask_px = 0;
    uint64_t ask_sz = 0;
    bool operator==(const Bbo&) const = default;
};

template <bool IsBid>
class SideMap {
  public:
    static constexpr bool kIsBid = IsBid;
    void add(Price px, uint32_t shares) {
        Level& l = m_[px];
        l.shares += shares;
        ++l.orders;
    }
    // Returns false if the level was missing or inconsistent (caller counts it as an anomaly).
    bool reduce(Price px, uint32_t shares, bool order_gone) {
        auto it = m_.find(px);
        if (it == m_.end()) return false;
        Level& l = it->second;
        bool ok = l.shares >= shares && (!order_gone || l.orders > 0);
        l.shares = l.shares >= shares ? l.shares - shares : 0;
        if (order_gone && l.orders > 0) --l.orders;
        if (l.orders == 0) {
            ok = ok && l.shares == 0;
            m_.erase(it);
        }
        return ok;
    }
    bool best(Price& px, Level& lv) const {
        if (m_.empty()) return false;
        px = m_.begin()->first;
        lv = m_.begin()->second;
        return true;
    }
    template <class F>
    void for_each(F&& f) const {  // best -> worst
        for (const auto& [px, lv] : m_) f(px, lv);
    }
    size_t size() const { return m_.size(); }
    bool empty() const { return m_.empty(); }

  private:
    using Cmp = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;
    std::map<Price, Level, Cmp> m_;
};

template <bool IsBid>
class SideVec {
  public:
    static constexpr bool kIsBid = IsBid;
    void add(Price px, uint32_t shares) {
        auto it = lower(px);
        if (it != v_.end() && it->first == px) {
            it->second.shares += shares;
            ++it->second.orders;
        } else {
            v_.insert(it, {px, Level{shares, 1}});
        }
    }
    bool reduce(Price px, uint32_t shares, bool order_gone) {
        auto it = lower(px);
        if (it == v_.end() || it->first != px) return false;
        Level& l = it->second;
        bool ok = l.shares >= shares && (!order_gone || l.orders > 0);
        l.shares = l.shares >= shares ? l.shares - shares : 0;
        if (order_gone && l.orders > 0) --l.orders;
        if (l.orders == 0) {
            ok = ok && l.shares == 0;
            v_.erase(it);
        }
        return ok;
    }
    bool best(Price& px, Level& lv) const {
        if (v_.empty()) return false;
        px = v_.back().first;
        lv = v_.back().second;
        return true;
    }
    template <class F>
    void for_each(F&& f) const {  // best -> worst
        for (auto it = v_.rbegin(); it != v_.rend(); ++it) f(it->first, it->second);
    }
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }

  private:
    using Entry = std::pair<Price, Level>;
    static bool worse(Price a, Price b) { return IsBid ? a < b : a > b; }
    typename std::vector<Entry>::iterator lower(Price px) {
        return std::lower_bound(v_.begin(), v_.end(), px,
                                [](const Entry& e, Price p) { return worse(e.first, p); });
    }
    std::vector<Entry> v_;  // sorted worst .. best
};

template <class Bid, class Ask>
struct Book {
    Bid bids;
    Ask asks;
    Bbo bbo() const {
        Bbo b;
        Price px = 0;
        Level lv;
        if (bids.best(px, lv)) b.bid_px = px, b.bid_sz = lv.shares;
        if (asks.best(px, lv)) b.ask_px = px, b.ask_sz = lv.shares;
        return b;
    }
    bool empty() const { return bids.empty() && asks.empty(); }
};

}  // namespace itch
