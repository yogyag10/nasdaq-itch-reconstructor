#pragma once
// BookBuilder: consumes parsed ITCH messages and reconstructs every symbol's book.
// This is a reconstructor, not a matching engine: Nasdaq's engine decides what executes; ITCH
// only reports the outcome (add / execute / cancel / delete / replace). We never match orders.
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "itch/book.hpp"
#include "itch/handler_base.hpp"
#include "itch/hash.hpp"
#include "itch/messages.hpp"
#include "itch/order_store.hpp"

#ifndef ITCH_DEBUG_INVARIANTS
#ifndef NDEBUG
#define ITCH_DEBUG_INVARIANTS 1
#else
#define ITCH_DEBUG_INVARIANTS 0
#endif
#endif

namespace itch {

struct TradeRec {
    uint64_t ts;
    Locate locate;
    char kind;  // 'E' executed, 'C' executed with price, 'P' non-cross trade
    Price price;
    uint32_t shares;
    uint64_t match;
    bool printable;
};

// Sink: where optional outputs go. Compile-time flags let the benchmark path pay nothing.
struct NullSink {
    static constexpr bool kWantsBbo = false;
    static constexpr bool kWantsTape = false;
    void on_symbol(Locate, const char (&)[8]) {}
    void on_bbo(uint64_t, Locate, const Bbo&) {}
    void on_trade(const TradeRec&) {}
    template <class H>
    void on_checkpoint(uint64_t, const H&) {}
};

// Counters for everything real data (or a malformed feed) can throw at us. None of these abort.
struct Anomalies {
    uint64_t unknown_exec = 0, unknown_exec_price = 0, unknown_cancel = 0, unknown_delete = 0,
             unknown_replace = 0;
    uint64_t duplicate_ref = 0, zero_share_add = 0;
    uint64_t exec_exceeds_remaining = 0, cancel_exceeds_remaining = 0;
    uint64_t level_inconsistent = 0;  // book level missing or totals disagreed on a reduce
    uint64_t crossed_after_msg = 0;   // messages that left best bid >= best ask
    uint64_t malformed = 0, unknown_type = 0;
    uint64_t total() const {
        return unknown_exec + unknown_exec_price + unknown_cancel + unknown_delete +
               unknown_replace + duplicate_ref + zero_share_add + exec_exceeds_remaining +
               cancel_exceeds_remaining + level_inconsistent + malformed + unknown_type;
    }
};

template <class Store, class BidSide, class AskSide, class Sink = NullSink>
class BookBuilder : public HandlerBase {
  public:
    using BookT = Book<BidSide, AskSide>;
    static constexpr size_t kLocates = 65536;

    explicit BookBuilder(Sink sink = Sink{}, size_t store_reserve = 1u << 16,
                         uint64_t checkpoint_every = 0)
        : store_(store_reserve),
          sink_(std::move(sink)),
          books_(kLocates),
          checkpoint_every_(checkpoint_every) {
        if constexpr (Sink::kWantsBbo) last_bbo_.resize(kLocates);
    }

    // ---- HandlerBase hooks -------------------------------------------------------------
    void begin(uint8_t type) {
        ++counts_[type];
        touched_ = kNone;
    }
    void end() {
        ++msg_index_;
        if (touched_ != kNone) after_book_change(touched_);
        if (checkpoint_every_ != 0 && msg_index_ % checkpoint_every_ == 0)
            sink_.on_checkpoint(msg_index_, *this);
    }
    void on_stock_directory(const StockDirectory& d) {
        std::memcpy(symbols_[d.h.locate].data(), d.stock, 8);
        has_symbol_.set(d.h.locate);
        sink_.on_symbol(d.h.locate, d.stock);
    }
    void on_add(const AddOrder& a) {
        if (a.ref == kReservedRef) {  // the flat store's empty-slot marker; see order_store.hpp
            ++anom_.malformed;
            return;
        }
        if (a.shares == 0) {
            ++anom_.zero_share_add;
            return;
        }
        if (!store_.insert(a.ref, OrderRec{a.price, a.shares, a.h.locate, a.side})) {
            ++anom_.duplicate_ref;
            return;
        }
        add_level(a.h.locate, a.side, a.price, a.shares);
        touched_ = a.h.locate;
        ts_ = a.h.ts;
    }
    void on_executed(const OrderExecuted& e) {
        OrderRec* r = store_.find(e.ref);
        if (!r) {
            ++anom_.unknown_exec;
            return;
        }
        const Price px = r->price;  // E executes at the resting order's price
        const uint32_t n = reduce_order(e.ref, r, e.shares, anom_.exec_exceeds_remaining);
        ts_ = e.h.ts;
        record_trade(e.h.ts, e.h.locate, 'E', px, n, e.match, true);
    }
    void on_executed_price(const OrderExecutedPrice& e) {
        OrderRec* r = store_.find(e.ref);
        if (!r) {
            ++anom_.unknown_exec_price;
            return;
        }
        // The book level is reduced at the order's own price; the print price may differ.
        const uint32_t n = reduce_order(e.ref, r, e.shares, anom_.exec_exceeds_remaining);
        ts_ = e.h.ts;
        record_trade(e.h.ts, e.h.locate, 'C', e.price, n, e.match, e.printable);
    }
    void on_cancel(const OrderCancel& c) {
        OrderRec* r = store_.find(c.ref);
        if (!r) {
            ++anom_.unknown_cancel;
            return;
        }
        ts_ = c.h.ts;
        reduce_order(c.ref, r, c.shares, anom_.cancel_exceeds_remaining);
    }
    void on_delete(const OrderDelete& d) {
        OrderRec* r = store_.find(d.ref);
        if (!r) {
            ++anom_.unknown_delete;
            return;
        }
        ts_ = d.h.ts;
        remove_order(d.ref, *r);
    }
    void on_replace(const OrderReplace& u) {
        if (u.new_ref == kReservedRef) {  // reject the whole message: the old order stays live
            ++anom_.malformed;
            return;
        }
        OrderRec* r = store_.find(u.old_ref);
        if (!r) {
            ++anom_.unknown_replace;
            return;
        }
        const OrderRec old = *r;  // side and symbol cannot change on a replace
        ts_ = u.h.ts;
        remove_order(u.old_ref, old);
        if (u.shares == 0) {
            ++anom_.zero_share_add;
            return;
        }
        if (!store_.insert(u.new_ref, OrderRec{u.price, u.shares, old.locate, old.side})) {
            ++anom_.duplicate_ref;
            return;
        }
        add_level(old.locate, old.side, u.price, u.shares);  // new priority: re-added at the back
    }
    void on_trade(const Trade& t) {  // non-displayed match: tape only, no book effect
        record_trade(t.h.ts, t.h.locate, 'P', t.price, t.shares, t.match, true);
    }
    void on_other(uint8_t) {}
    void on_unknown(uint8_t) { ++anom_.unknown_type; }
    void on_malformed(uint8_t) { ++anom_.malformed; }

    // ---- Inspection --------------------------------------------------------------------
    const BookT& book(Locate l) const { return books_[l]; }
    const std::array<char, 8>& symbol(Locate l) const { return symbols_[l]; }
    bool has_symbol(Locate l) const { return has_symbol_.test(l); }
    const std::array<uint64_t, 256>& counts() const { return counts_; }
    const Anomalies& anomalies() const { return anom_; }
    const Store& store() const { return store_; }
    Sink& sink() { return sink_; }
    uint64_t messages() const { return msg_index_; }
    uint64_t printable_shares() const { return printable_shares_; }
    uint64_t nonprintable_shares() const { return nonprintable_shares_; }

    // Hash of every non-empty book, best->worst per side, locates ascending. Independent of the
    // Store/Side implementation, so differing designs must agree on it.
    uint64_t state_hash() const {
        Fnv1a64 h;
        for (size_t l = 0; l < kLocates; ++l) {
            const BookT& b = books_[l];
            if (b.empty()) continue;
            h.u32(static_cast<uint32_t>(l));
            h.u8('B');
            b.bids.for_each([&](Price px, const Level& lv) {
                h.u32(px);
                h.u64(lv.shares);
                h.u32(lv.orders);
            });
            h.u8('A');
            b.asks.for_each([&](Price px, const Level& lv) {
                h.u32(px);
                h.u64(lv.shares);
                h.u32(lv.orders);
            });
        }
        h.u64(store_.size());
        return h.value();
    }

    // Full-scan invariant check: every level's shares/orders equal the sum over live orders,
    // no level exists without orders, no order has zero shares. O(orders + levels); tests call
    // it after every message, debug replays every N messages. Returns "" when consistent.
    std::string verify() const {
        struct Key {
            Locate loc;
            Side side;
            Price px;
            bool operator<(const Key& o) const {
                return std::tie(loc, side, px) < std::tie(o.loc, o.side, o.px);
            }
        };
        std::map<Key, Level> sums;
        std::string err;
        store_.for_each([&](OrderRef ref, const OrderRec& r) {
            if (r.shares == 0 && err.empty())
                err = "order " + std::to_string(ref) + " has 0 shares";
            Level& l = sums[Key{r.locate, r.side, r.price}];
            l.shares += r.shares;
            ++l.orders;
        });
        size_t levels = 0;
        for (size_t l = 0; l < kLocates && err.empty(); ++l) {
            auto chk = [&](Side s, const auto& side) {
                side.for_each([&](Price px, const Level& lv) {
                    ++levels;
                    auto it = sums.find(Key{static_cast<Locate>(l), s, px});
                    if (it == sums.end() || it->second.shares != lv.shares ||
                        it->second.orders != lv.orders)
                        if (err.empty())
                            err = "level mismatch locate " + std::to_string(l) + " px " +
                                  std::to_string(px);
                });
            };
            chk(Side::Buy, books_[l].bids);
            chk(Side::Sell, books_[l].asks);
        }
        if (err.empty() && levels != sums.size()) err = "order(s) with no matching book level";
        return err;
    }

  private:
    static constexpr Locate kNone = 0;  // locate 0 is "not stock dependent" per spec
    // Remove up to n shares from order r at the book; returns shares actually removed.
    uint32_t reduce_order(OrderRef ref, OrderRec* r, uint32_t n, uint64_t& exceeds_counter) {
        if (n > r->shares) {
            ++exceeds_counter;
            n = r->shares;
        }
        const OrderRec snap = *r;
        const bool gone = n == snap.shares;
        if (!reduce_level(snap.locate, snap.side, snap.price, n, gone)) ++anom_.level_inconsistent;
        if (gone)
            store_.erase(ref);
        else
            r->shares -= n;
        touched_ = snap.locate;
        return n;
    }
    // By value on purpose: erase() invalidates any reference into the store (use-after-free for
    // the node-based map, a silently shifted neighbour for the flat map).
    void remove_order(OrderRef ref, OrderRec r) {
        if (!reduce_level(r.locate, r.side, r.price, r.shares, true)) ++anom_.level_inconsistent;
        store_.erase(ref);
        touched_ = r.locate;
    }
    void add_level(Locate l, Side s, Price px, uint32_t n) {
        if (s == Side::Buy)
            books_[l].bids.add(px, n);
        else
            books_[l].asks.add(px, n);
        touched_ = l;
    }
    bool reduce_level(Locate l, Side s, Price px, uint32_t n, bool gone) {
        return s == Side::Buy ? books_[l].bids.reduce(px, n, gone)
                              : books_[l].asks.reduce(px, n, gone);
    }
    void record_trade(uint64_t ts, Locate l, char kind, Price px, uint32_t n, uint64_t match,
                      bool printable) {
        (printable ? printable_shares_ : nonprintable_shares_) += n;
        if constexpr (Sink::kWantsTape)
            sink_.on_trade(TradeRec{ts, l, kind, px, n, match, printable});
    }
    void after_book_change(Locate l) {
        const BookT& b = books_[l];
        const Bbo cur = b.bbo();
        if (cur.bid_sz != 0 && cur.ask_sz != 0 && cur.bid_px >= cur.ask_px)
            ++anom_.crossed_after_msg;
        if constexpr (Sink::kWantsBbo) {
            if (!(cur == last_bbo_[l])) {
                last_bbo_[l] = cur;
                sink_.on_bbo(ts_, l, cur);
            }
        }
#if ITCH_DEBUG_INVARIANTS
        assert_touched_ok(b);
#endif
    }
#if ITCH_DEBUG_INVARIANTS
    void assert_touched_ok(const BookT& b) const {
        Price px = 0;
        Level lv;
        if (b.bids.best(px, lv)) {
            if (lv.shares == 0 || lv.orders == 0) __builtin_trap();
        }
        if (b.asks.best(px, lv)) {
            if (lv.shares == 0 || lv.orders == 0) __builtin_trap();
        }
    }
#endif

    Store store_;
    Sink sink_;
    std::vector<BookT> books_;
    std::vector<Bbo> last_bbo_;
    std::array<std::array<char, 8>, kLocates> symbols_{};
    struct Bits {
        std::array<uint64_t, kLocates / 64> w{};
        void set(size_t i) { w[i >> 6] |= 1ull << (i & 63); }
        bool test(size_t i) const { return (w[i >> 6] >> (i & 63)) & 1; }
    } has_symbol_;
    std::array<uint64_t, 256> counts_{};
    Anomalies anom_;
    uint64_t msg_index_ = 0;
    uint64_t checkpoint_every_;
    uint64_t ts_ = 0;
    uint64_t printable_shares_ = 0, nonprintable_shares_ = 0;
    Locate touched_ = kNone;
};

}  // namespace itch
