#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>

#include "encoder.hpp"
#include "itch/book_handler.hpp"
#include "itch/parser.hpp"
#include "synth.hpp"

using namespace itch;
using namespace itch::test;

namespace {

struct RecSink {
    static constexpr bool kWantsBbo = true;
    static constexpr bool kWantsTape = true;
    struct S {
        std::vector<TradeRec> tape;
        std::vector<std::pair<Locate, Bbo>> bbo;
    };
    std::shared_ptr<S> s = std::make_shared<S>();
    void on_symbol(Locate, const char (&)[8]) {}
    void on_bbo(uint64_t, Locate l, const Bbo& b) { s->bbo.emplace_back(l, b); }
    void on_trade(const TradeRec& t) { s->tape.push_back(t); }
    template <class H>
    void on_checkpoint(uint64_t, const H&) {}
};

template <class Store, class Bid, class Ask>
struct DesignT {
    using Handler = BookBuilder<Store, Bid, Ask, RecSink>;
};
using Designs = ::testing::Types<DesignT<StdOrderStore, SideMap<true>, SideMap<false>>,
                                 DesignT<StdOrderStore, SideVec<true>, SideVec<false>>,
                                 DesignT<FlatOrderStore, SideMap<true>, SideMap<false>>,
                                 DesignT<FlatOrderStore, SideVec<true>, SideVec<false>>>;

template <class D>
class BookTest : public ::testing::Test {
  protected:
    using H = typename D::Handler;
    BookTest() : h(std::make_unique<H>(RecSink{}, 64)) {}
    // Feed one message and check the full-scan invariants after it, as the spec'd debug mode does.
    void send(const Buf& m) {
        parse_message({m.data(), m.size()}, *h);
        ASSERT_EQ(h->verify(), "") << "after message type " << static_cast<char>(m[0]);
    }
    Bbo bbo(Locate l = 1) const { return h->book(l).bbo(); }
    const RecSink::S& rec() { return *h->sink().s; }
    std::unique_ptr<H> h;
};
TYPED_TEST_SUITE(BookTest, Designs);

}  // namespace

TYPED_TEST(BookTest, AddThenFullExecute) {
    this->send(add(1, 10, 1, 'B', 300, "AAPL", 1000000));
    EXPECT_EQ(this->bbo().bid_px, 1000000u);
    EXPECT_EQ(this->bbo().bid_sz, 300u);
    this->send(executed(1, 11, 1, 300, 9));
    EXPECT_EQ(this->bbo().bid_sz, 0u);  // level gone
    EXPECT_TRUE(this->h->book(1).empty());
    EXPECT_EQ(this->h->store().size(), 0u);  // only live orders are stored
    ASSERT_EQ(this->rec().tape.size(), 1u);
    EXPECT_EQ(this->rec().tape[0].kind, 'E');
    EXPECT_EQ(this->rec().tape[0].price, 1000000u);  // E prints at the order's own price
    EXPECT_EQ(this->rec().tape[0].shares, 300u);
}

TYPED_TEST(BookTest, PartialExecuteThenCancel) {
    this->send(add(1, 10, 1, 'S', 500, "AAPL", 1010000));
    this->send(executed(1, 11, 1, 200, 1));
    EXPECT_EQ(this->bbo().ask_sz, 300u);
    this->send(cancel(1, 12, 1, 100));
    EXPECT_EQ(this->bbo().ask_sz, 200u);
    this->send(executed(1, 13, 1, 200, 2));
    EXPECT_TRUE(this->h->book(1).empty());
    EXPECT_EQ(this->h->store().size(), 0u);
}

TYPED_TEST(BookTest, ReplaceNewRefNewPricePriorityLost) {
    this->send(add(1, 10, 1, 'B', 100, "AAPL", 1000000));
    this->send(add(1, 11, 2, 'B', 100, "AAPL", 1000000));  // same level, behind order 1
    this->send(replace(1, 12, 1, 3, 150, 1000100));
    // Old ref is dead, new ref is live at the new price on the same side.
    EXPECT_EQ(this->h->store().size(), 2u);
    this->send(del(1, 13, 1));
    EXPECT_EQ(this->h->anomalies().unknown_delete, 1u);
    EXPECT_EQ(this->bbo().bid_px, 1000100u);
    EXPECT_EQ(this->bbo().bid_sz, 150u);
    this->send(del(1, 14, 3));
    EXPECT_EQ(this->bbo().bid_px, 1000000u);  // order 2 remains at the old level
    EXPECT_EQ(this->bbo().bid_sz, 100u);
}

TYPED_TEST(BookTest, ReplaceKeepsSideAndSymbol) {
    this->send(add(7, 10, 1, 'S', 100, "ILQ", 2000000));
    this->send(replace(7, 11, 1, 2, 100, 2000100));
    EXPECT_EQ(this->bbo(7).ask_px, 2000100u);
    EXPECT_EQ(this->bbo(7).bid_sz, 0u);
    EXPECT_TRUE(this->h->book(1).empty());
}

TYPED_TEST(BookTest, DeleteOfLastOrderRemovesLevel) {
    this->send(add(1, 10, 1, 'B', 100, "AAPL", 1000000));
    this->send(add(1, 11, 2, 'B', 200, "AAPL", 999900));
    this->send(add(1, 12, 3, 'B', 50, "AAPL", 999900));
    this->send(del(1, 13, 1));
    EXPECT_EQ(this->bbo().bid_px, 999900u);
    EXPECT_EQ(this->bbo().bid_sz, 250u);
    this->send(del(1, 14, 2));
    EXPECT_EQ(this->bbo().bid_sz, 50u);
    this->send(del(1, 15, 3));
    EXPECT_TRUE(this->h->book(1).empty());  // level must disappear, not linger at 0 shares
    EXPECT_EQ(this->h->book(1).bids.size(), 0u);
}

TYPED_TEST(BookTest, ExecutionsOnBothSides) {
    this->send(add(1, 10, 1, 'B', 100, "AAPL", 1000000));
    this->send(add(1, 11, 2, 'S', 100, "AAPL", 1000100));
    this->send(executed(1, 12, 1, 40, 1));
    this->send(executed(1, 13, 2, 70, 2));
    EXPECT_EQ(this->bbo().bid_sz, 60u);
    EXPECT_EQ(this->bbo().ask_sz, 30u);
    EXPECT_EQ(this->h->printable_shares(), 110u);
}

TYPED_TEST(BookTest, ExecutedWithPriceReducesAtOrderPriceButPrintsAtExecPrice) {
    this->send(add(1, 10, 1, 'B', 200, "AAPL", 1000000));
    this->send(executed_price(1, 11, 1, 100, 1, 'Y', 1000500));
    this->send(executed_price(1, 12, 1, 50, 2, 'N', 1000500));
    EXPECT_EQ(this->bbo().bid_px, 1000000u);  // book price unchanged by exec price
    EXPECT_EQ(this->bbo().bid_sz, 50u);
    ASSERT_EQ(this->rec().tape.size(), 2u);
    EXPECT_EQ(this->rec().tape[0].price, 1000500u);
    EXPECT_TRUE(this->rec().tape[0].printable);
    EXPECT_FALSE(this->rec().tape[1].printable);
    // Non-printable shares still leave the book but must not count toward volume.
    EXPECT_EQ(this->h->printable_shares(), 100u);
    EXPECT_EQ(this->h->nonprintable_shares(), 50u);
}

TYPED_TEST(BookTest, UnknownReferencesAreCountedNotFatal) {
    this->send(executed(1, 1, 42, 10, 1));
    this->send(executed_price(1, 2, 42, 10, 2, 'Y', 1));
    this->send(cancel(1, 3, 42, 10));
    this->send(del(1, 4, 42));
    this->send(replace(1, 5, 42, 43, 10, 1));
    const auto& a = this->h->anomalies();
    EXPECT_EQ(a.unknown_exec, 1u);
    EXPECT_EQ(a.unknown_exec_price, 1u);
    EXPECT_EQ(a.unknown_cancel, 1u);
    EXPECT_EQ(a.unknown_delete, 1u);
    EXPECT_EQ(a.unknown_replace, 1u);
    EXPECT_TRUE(this->h->book(1).empty());
    EXPECT_EQ(this->h->store().size(), 0u);
}

TYPED_TEST(BookTest, OverExecutionAndOverCancelClampAndCount) {
    this->send(add(1, 1, 1, 'B', 100, "AAPL", 1000));
    this->send(cancel(1, 2, 1, 500));  // more than remaining
    EXPECT_EQ(this->h->anomalies().cancel_exceeds_remaining, 1u);
    EXPECT_TRUE(this->h->book(1).empty());
    this->send(add(1, 3, 2, 'B', 100, "AAPL", 1000));
    this->send(executed(1, 4, 2, 500, 1));
    EXPECT_EQ(this->h->anomalies().exec_exceeds_remaining, 1u);
    EXPECT_EQ(this->h->printable_shares(), 100u);  // clamped to what was actually resting
}

TYPED_TEST(BookTest, DuplicateRefAndZeroShareAddsIgnored) {
    this->send(add(1, 1, 1, 'B', 100, "AAPL", 1000));
    this->send(add(1, 2, 1, 'B', 999, "AAPL", 1000));  // reused live ref
    this->send(add(1, 3, 2, 'B', 0, "AAPL", 1000));    // zero shares
    EXPECT_EQ(this->h->anomalies().duplicate_ref, 1u);
    EXPECT_EQ(this->h->anomalies().zero_share_add, 1u);
    EXPECT_EQ(this->bbo().bid_sz, 100u);
}

TYPED_TEST(BookTest, BestBidAndAskTrackedAcrossLevels) {
    this->send(add(1, 1, 1, 'B', 10, "A", 1000));
    this->send(add(1, 2, 2, 'B', 10, "A", 1200));
    this->send(add(1, 3, 3, 'B', 10, "A", 1100));
    this->send(add(1, 4, 4, 'S', 10, "A", 1500));
    this->send(add(1, 5, 5, 'S', 10, "A", 1300));
    this->send(add(1, 6, 6, 'S', 10, "A", 1400));
    EXPECT_EQ(this->bbo().bid_px, 1200u);
    EXPECT_EQ(this->bbo().ask_px, 1300u);
    this->send(del(1, 7, 2));
    this->send(del(1, 8, 5));
    EXPECT_EQ(this->bbo().bid_px, 1100u);
    EXPECT_EQ(this->bbo().ask_px, 1400u);
    EXPECT_EQ(this->h->anomalies().crossed_after_msg, 0u);
}

TYPED_TEST(BookTest, CrossedBookIsCountedNotAsserted) {
    this->send(add(1, 1, 1, 'B', 10, "A", 1500));
    this->send(add(1, 2, 2, 'S', 10, "A", 1400));
    EXPECT_EQ(this->h->anomalies().crossed_after_msg, 1u);
}

TYPED_TEST(BookTest, NonCrossTradeRecordedWithoutBookEffect) {
    this->send(add(1, 1, 1, 'B', 100, "AAPL", 1000));
    this->send(trade(1, 2, 'B', 25, "AAPL", 1000, 5));
    EXPECT_EQ(this->bbo().bid_sz, 100u);
    ASSERT_EQ(this->rec().tape.size(), 1u);
    EXPECT_EQ(this->rec().tape[0].kind, 'P');
}

TYPED_TEST(BookTest, BboLogRecordsOnlyChanges) {
    this->send(add(1, 1, 1, 'B', 100, "AAPL", 1000));  // bbo changes
    this->send(add(1, 2, 2, 'B', 100, "AAPL", 900));   // behind the best: no change
    this->send(add(1, 3, 3, 'B', 50, "AAPL", 1000));   // size at best changes
    EXPECT_EQ(this->rec().bbo.size(), 2u);
}

TYPED_TEST(BookTest, SymbolsAreIndependent) {
    this->send(add(1, 1, 1, 'B', 100, "A", 1000));
    this->send(add(2, 2, 2, 'B', 300, "B", 2000));
    EXPECT_EQ(this->bbo(1).bid_sz, 100u);
    EXPECT_EQ(this->bbo(2).bid_sz, 300u);
    this->send(del(1, 3, 1));
    EXPECT_TRUE(this->h->book(1).empty());
    EXPECT_EQ(this->bbo(2).bid_sz, 300u);
}

TYPED_TEST(BookTest, SyntheticDayKeepsInvariantsEveryMessage) {
    SynthConfig cfg;
    cfg.messages = 4000;
    cfg.symbols = 6;
    const auto day = make_day(cfg);
    FrameCursor c({day.bytes().data(), day.bytes().size()});
    Bytes m;
    size_t i = 0;
    while (c.next(m)) {
        parse_message(m, *this->h);
        if (++i % 7 == 0 || i < 300) ASSERT_EQ(this->h->verify(), "") << "msg " << i;
    }
    EXPECT_EQ(this->h->verify(), "");
    EXPECT_EQ(this->h->anomalies().malformed, 0u);
    EXPECT_EQ(this->h->anomalies().level_inconsistent, 0u);
    EXPECT_GT(this->h->anomalies().unknown_exec + this->h->anomalies().unknown_cancel +
                  this->h->anomalies().unknown_delete + this->h->anomalies().unknown_replace +
                  this->h->anomalies().unknown_exec_price,
              0u);  // the generator injects unknown references on purpose
}

// Regression for a use-after-erase in remove_order: if the touched symbol is misattributed the BBO
// log silently misses an update. After EVERY message, the BBO the sink last saw for each symbol
// must equal the real book (checking only the end state lets later messages mask a missed update).
TYPED_TEST(BookTest, BboLogTracksTheTrueBookAfterEveryMessage) {
    SynthConfig cfg;
    cfg.messages = 20000;
    cfg.symbols = 9;
    const auto day = make_day(cfg);
    std::map<Locate, Bbo> last;
    size_t seen = 0;
    FrameCursor c({day.bytes().data(), day.bytes().size()});
    Bytes m;
    size_t i = 0;
    while (c.next(m)) {
        parse_message(m, *this->h);
        for (; seen < this->rec().bbo.size(); ++seen)
            last[this->rec().bbo[seen].first] = this->rec().bbo[seen].second;
        for (Locate l = 1; l <= cfg.symbols; ++l) {
            auto it = last.find(l);
            const Bbo got = it == last.end() ? Bbo{} : it->second;
            ASSERT_EQ(got, this->h->book(l).bbo()) << "locate " << l << " after message " << i;
        }
        ++i;
    }
}

// All four designs must agree bit-for-bit on the final state, and rerunning must reproduce it.
TEST(Determinism, DesignsAgreeAndRunsRepeat) {
    SynthConfig cfg;
    cfg.messages = 60000;
    cfg.symbols = 12;
    const auto day = make_day(cfg);
    auto run = [&](auto tag) {
        using D = decltype(tag);
        FrameStats st;
        auto h = std::make_unique<typename D::Handler>(RecSink{}, 1u << 10);
        consume_frames({day.bytes().data(), day.bytes().size()}, *h, st);
        EXPECT_EQ(st.messages, day.count());
        return h->state_hash();
    };
    const uint64_t a = run(DesignT<StdOrderStore, SideMap<true>, SideMap<false>>{});
    EXPECT_EQ(a, run(DesignT<StdOrderStore, SideMap<true>, SideMap<false>>{}));
    EXPECT_EQ(a, run(DesignT<StdOrderStore, SideVec<true>, SideVec<false>>{}));
    EXPECT_EQ(a, run(DesignT<FlatOrderStore, SideMap<true>, SideMap<false>>{}));
    EXPECT_EQ(a, run(DesignT<FlatOrderStore, SideVec<true>, SideVec<false>>{}));
}
