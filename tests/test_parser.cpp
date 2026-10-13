#include <gtest/gtest.h>

#include <vector>

#include "encoder.hpp"
#include "itch/parser.hpp"

using namespace itch;
using namespace itch::test;

namespace {

struct Rec : HandlerBase {
    std::vector<char> types;
    std::vector<AddOrder> adds;
    std::vector<OrderExecutedPrice> execs_price;
    std::vector<OrderReplace> replaces;
    std::vector<Trade> trades;
    std::vector<CrossTrade> crosses;
    int malformed = 0, unknown = 0, other = 0, ends = 0;
    void begin(uint8_t t) { types.push_back(static_cast<char>(t)); }
    void end() { ++ends; }
    void on_add(const AddOrder& a) { adds.push_back(a); }
    void on_executed_price(const OrderExecutedPrice& e) { execs_price.push_back(e); }
    void on_replace(const OrderReplace& r) { replaces.push_back(r); }
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_cross(const CrossTrade& c) { crosses.push_back(c); }
    void on_malformed(uint8_t) { ++malformed; }
    void on_unknown(uint8_t) { ++unknown; }
    void on_other(uint8_t) { ++other; }
};

Bytes B(const Buf& b) {
    return {b.data(), b.size()};
}

}  // namespace

// Lengths below are listed independently of messages.hpp, straight from the spec offset tables
// (last field offset + length). The first eight are the ones in the project brief.
TEST(Lengths, MatchOfficialSpec) {
    const struct {
        char t;
        unsigned n;
    } expect[] = {{'A', 36}, {'F', 40}, {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19},
                  {'U', 35}, {'P', 44}, {'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20},
                  {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28}, {'J', 35}, {'h', 21},
                  {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    for (auto e : expect) EXPECT_EQ(kMsgLength[static_cast<uint8_t>(e.t)], e.n) << e.t;
    unsigned defined = 0;
    for (auto n : kMsgLength) defined += n != 0;
    EXPECT_EQ(defined, 23u);
}

TEST(Encoder, ProducesSpecLengths) {
    EXPECT_EQ(system_event(1, 'O').size(), 12u);
    EXPECT_EQ(stock_directory(1, 1, "X").size(), 39u);
    EXPECT_EQ(trading_action(1, 1, "X", 'H').size(), 25u);
    EXPECT_EQ(add(1, 1, 1, 'B', 1, "X", 1).size(), 36u);
    EXPECT_EQ(add_mpid(1, 1, 1, 'B', 1, "X", 1, "NSDQ").size(), 40u);
    EXPECT_EQ(executed(1, 1, 1, 1, 1).size(), 31u);
    EXPECT_EQ(executed_price(1, 1, 1, 1, 1, 'Y', 1).size(), 36u);
    EXPECT_EQ(cancel(1, 1, 1, 1).size(), 23u);
    EXPECT_EQ(del(1, 1, 1).size(), 19u);
    EXPECT_EQ(replace(1, 1, 1, 2, 1, 1).size(), 35u);
    EXPECT_EQ(trade(1, 1, 'B', 1, "X", 1, 1).size(), 44u);
    EXPECT_EQ(cross(1, 1, 1, "X", 1, 1, 'O').size(), 40u);
    EXPECT_EQ(broken(1, 1, 1).size(), 19u);
}

TEST(Endian, KnownBytes) {
    const std::byte d[8] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                            std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    EXPECT_EQ(be16(d), 0x0102u);
    EXPECT_EQ(be32(d), 0x01020304u);
    EXPECT_EQ(be48(d), 0x010203040506ull);
    EXPECT_EQ(be64(d), 0x0102030405060708ull);
    // High bit set must not sign-extend.
    const std::byte hi[8] = {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                             std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    EXPECT_EQ(be32(hi), 0xffffffffu);
    EXPECT_EQ(be64(hi), ~0ull);
}

TEST(Decode, AddOrderFields) {
    Rec r;
    const auto m =
        add(513, 34'200'000'000'123ull, 0x0102030405060708ull, 'S', 700, "AAPL", 1'234'500);
    EXPECT_EQ(parse_message(B(m), r), ParseStatus::Ok);
    ASSERT_EQ(r.adds.size(), 1u);
    const auto& a = r.adds[0];
    EXPECT_EQ(a.h.locate, 513);
    EXPECT_EQ(a.h.ts, 34'200'000'000'123ull);
    EXPECT_EQ(a.ref, 0x0102030405060708ull);
    EXPECT_EQ(a.side, Side::Sell);
    EXPECT_EQ(a.shares, 700u);
    EXPECT_EQ(std::string(a.stock, 8), "AAPL    ");
    EXPECT_EQ(a.price, 1'234'500u);  // $123.45 as Price(4), kept as an integer
    EXPECT_FALSE(a.has_mpid);
}

TEST(Decode, AddWithMpid) {
    Rec r;
    const auto m = add_mpid(1, 5, 9, 'B', 100, "MSFT", 100, "GSCO");
    EXPECT_EQ(parse_message(B(m), r), ParseStatus::Ok);
    ASSERT_EQ(r.adds.size(), 1u);
    EXPECT_TRUE(r.adds[0].has_mpid);
    EXPECT_EQ(std::string(r.adds[0].mpid, 4), "GSCO");
}

TEST(Decode, ExecutedWithPricePrintableFlag) {
    Rec r;
    const auto y = executed_price(1, 5, 9, 100, 77, 'Y', 2000);
    const auto n = executed_price(1, 5, 9, 100, 78, 'N', 2000);
    parse_message(B(y), r);
    parse_message(B(n), r);
    ASSERT_EQ(r.execs_price.size(), 2u);
    EXPECT_TRUE(r.execs_price[0].printable);
    EXPECT_FALSE(r.execs_price[1].printable);
    EXPECT_EQ(r.execs_price[0].price, 2000u);
    EXPECT_EQ(r.execs_price[0].match, 77u);
}

TEST(Decode, ReplaceTradeCross) {
    Rec r;
    parse_message(B(replace(2, 7, 100, 101, 300, 4242)), r);
    parse_message(B(trade(2, 8, 'B', 50, "ZZZ", 99, 5)), r);
    parse_message(B(cross(2, 9, 123456789012ull, "ZZZ", 99, 6, 'C')), r);
    ASSERT_EQ(r.replaces.size(), 1u);
    EXPECT_EQ(r.replaces[0].old_ref, 100u);
    EXPECT_EQ(r.replaces[0].new_ref, 101u);
    EXPECT_EQ(r.replaces[0].shares, 300u);
    EXPECT_EQ(r.replaces[0].price, 4242u);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].ref, 0u);
    EXPECT_EQ(r.trades[0].match, 5u);
    ASSERT_EQ(r.crosses.size(), 1u);
    EXPECT_EQ(r.crosses[0].shares, 123456789012ull);
    EXPECT_EQ(r.crosses[0].cross_type, 'C');
}

TEST(Malformed, WrongLengthUnknownTypeBadFields) {
    Rec r;
    auto short_add = add(1, 1, 1, 'B', 1, "X", 1);
    short_add.pop_back();
    EXPECT_EQ(parse_message(B(short_add), r), ParseStatus::BadLength);
    auto long_del = del(1, 1, 1);
    long_del.push_back(std::byte{0});
    EXPECT_EQ(parse_message(B(long_del), r), ParseStatus::BadLength);
    const Buf unknown = {std::byte{'z'}, std::byte{0}};
    EXPECT_EQ(parse_message(B(unknown), r), ParseStatus::Unknown);
    EXPECT_EQ(parse_message(Bytes{}, r), ParseStatus::BadLength);
    auto bad_side = add(1, 1, 1, 'Q', 1, "X", 1);  // side must be B or S
    EXPECT_EQ(parse_message(B(bad_side), r), ParseStatus::BadField);
    auto bad_print = executed_price(1, 1, 1, 1, 1, 'Z', 1);  // printable must be Y or N
    EXPECT_EQ(parse_message(B(bad_print), r), ParseStatus::BadField);
    EXPECT_EQ(r.malformed, 5);
    EXPECT_EQ(r.unknown, 1);
    EXPECT_EQ(r.ends, 6);  // every frame is bracketed by begin/end even when malformed
    EXPECT_TRUE(r.adds.empty());
}

TEST(Other, KnownTypesAreCounted) {
    Rec r;
    EXPECT_EQ(parse_message(B(broken(1, 1, 1)), r), ParseStatus::Ok);
    EXPECT_EQ(parse_message(B(system_event(1, 'O')), r), ParseStatus::Ok);
    // 'I' (NOII) has no dedicated callback: 50 zero-ish bytes with the right type + length.
    Buf noii(50, std::byte{0});
    noii[0] = std::byte{'I'};
    EXPECT_EQ(parse_message(B(noii), r), ParseStatus::Ok);
    EXPECT_EQ(r.other, 1);
}

TEST(Framer, IteratesFramesAndStopsAtPartialTail) {
    Stream s;
    s.add(system_event(1, 'O')).add(del(1, 2, 3));
    Buf d = s.bytes();
    const size_t clean = d.size();
    d.push_back(std::byte{0});  // start of a frame header, one byte only
    FrameCursor c({d.data(), d.size()});
    Bytes m;
    int n = 0;
    while (c.next(m)) ++n;
    EXPECT_EQ(n, 2);
    EXPECT_EQ(c.consumed(), clean);

    Buf t = s.bytes();
    t.pop_back();  // truncate the last payload
    FrameCursor c2({t.data(), t.size()});
    n = 0;
    while (c2.next(m)) ++n;
    EXPECT_EQ(n, 1);
}

TEST(Framer, ZeroLengthFramesSkipped) {
    Buf d = {std::byte{0}, std::byte{0}};
    const auto m = del(1, 1, 1);
    d.push_back(std::byte{0});
    d.push_back(static_cast<std::byte>(m.size()));
    d.insert(d.end(), m.begin(), m.end());
    Rec r;
    FrameStats st;
    consume_frames({d.data(), d.size()}, r, st);
    EXPECT_EQ(st.messages, 1u);
    EXPECT_EQ(st.zero_length_frames, 1u);
}

TEST(Framer, ArbitraryBytesNeverCrash) {
    // Cheap in-tree stand-in for the fuzzer: every prefix of a mixed stream and bit-flipped
    // copies must parse without UB (run under ASan/UBSan in CI).
    Stream s;
    s.add(stock_directory(1, 1, "AAPL"))
        .add(add(1, 2, 1, 'B', 100, "AAPL", 1000))
        .add(replace(1, 3, 1, 2, 50, 999))
        .add(trade(1, 4, 'B', 5, "AAPL", 1000, 1))
        .add(cross(1, 5, 5, "AAPL", 1000, 2, 'O'));
    Rec r;
    for (size_t n = 0; n <= s.bytes().size(); ++n) {
        FrameStats st;
        consume_frames({s.bytes().data(), n}, r, st);
    }
    for (size_t i = 0; i < s.bytes().size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            Buf c = s.bytes();
            c[i] ^= static_cast<std::byte>(1 << bit);
            FrameStats st;
            consume_frames({c.data(), c.size()}, r, st);
        }
    }
    SUCCEED();
}
