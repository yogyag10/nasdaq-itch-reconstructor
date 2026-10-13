#pragma once
// Test-only ITCH encoder. Written independently of the parser (own big-endian writers, own
// offsets taken from the spec tables) so round-trip tests can catch layout mistakes in either.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace itch::test {

using Buf = std::vector<std::byte>;

class W {
  public:
    W& u8(uint8_t v) { return push(v); }
    W& ch(char c) { return push(static_cast<uint8_t>(c)); }
    W& u16(uint16_t v) { return be(v, 2); }
    W& u32(uint32_t v) { return be(v, 4); }
    W& u48(uint64_t v) { return be(v, 6); }
    W& u64(uint64_t v) { return be(v, 8); }
    W& alpha(const std::string& s, size_t n) {  // left-justified, space padded
        for (size_t i = 0; i < n; ++i) ch(i < s.size() ? s[i] : ' ');
        return *this;
    }
    W& head(char type, uint16_t locate, uint64_t ts) {
        return ch(type).u16(locate).u16(0).u48(ts);  // tracking number 0
    }
    Buf done() { return std::move(b_); }

  private:
    W& push(uint8_t v) {
        b_.push_back(static_cast<std::byte>(v));
        return *this;
    }
    W& be(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) push(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Buf b_;
};

inline Buf system_event(uint64_t ts, char code) {
    return W().head('S', 0, ts).ch(code).done();
}
inline Buf stock_directory(uint16_t loc, uint64_t ts, const std::string& sym) {
    return W()
        .head('R', loc, ts)
        .alpha(sym, 8)
        .ch('Q')
        .ch('N')
        .u32(100)
        .ch('Y')
        .ch('P')
        .ch('N')
        .ch(' ')
        .ch('1')
        .ch('N')
        .ch(' ')
        .ch(' ')
        .ch('N')
        .u32(1)
        .ch('N')
        .done();  // 39 bytes total
}
inline Buf trading_action(uint16_t loc, uint64_t ts, const std::string& sym, char state) {
    return W().head('H', loc, ts).alpha(sym, 8).ch(state).ch(' ').alpha("", 4).done();
}
inline Buf add(uint16_t loc, uint64_t ts, uint64_t ref, char side, uint32_t shares,
               const std::string& sym, uint32_t price) {
    return W().head('A', loc, ts).u64(ref).ch(side).u32(shares).alpha(sym, 8).u32(price).done();
}
inline Buf add_mpid(uint16_t loc, uint64_t ts, uint64_t ref, char side, uint32_t shares,
                    const std::string& sym, uint32_t price, const std::string& mpid) {
    return W()
        .head('F', loc, ts)
        .u64(ref)
        .ch(side)
        .u32(shares)
        .alpha(sym, 8)
        .u32(price)
        .alpha(mpid, 4)
        .done();
}
inline Buf executed(uint16_t loc, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match) {
    return W().head('E', loc, ts).u64(ref).u32(shares).u64(match).done();
}
inline Buf executed_price(uint16_t loc, uint64_t ts, uint64_t ref, uint32_t shares, uint64_t match,
                          char printable, uint32_t price) {
    return W().head('C', loc, ts).u64(ref).u32(shares).u64(match).ch(printable).u32(price).done();
}
inline Buf cancel(uint16_t loc, uint64_t ts, uint64_t ref, uint32_t shares) {
    return W().head('X', loc, ts).u64(ref).u32(shares).done();
}
inline Buf del(uint16_t loc, uint64_t ts, uint64_t ref) {
    return W().head('D', loc, ts).u64(ref).done();
}
inline Buf replace(uint16_t loc, uint64_t ts, uint64_t old_ref, uint64_t new_ref, uint32_t shares,
                   uint32_t price) {
    return W().head('U', loc, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price).done();
}
inline Buf trade(uint16_t loc, uint64_t ts, char side, uint32_t shares, const std::string& sym,
                 uint32_t price, uint64_t match) {
    return W()
        .head('P', loc, ts)
        .u64(0)
        .ch(side)
        .u32(shares)
        .alpha(sym, 8)
        .u32(price)
        .u64(match)
        .done();
}
inline Buf cross(uint16_t loc, uint64_t ts, uint64_t shares, const std::string& sym, uint32_t price,
                 uint64_t match, char type) {
    return W().head('Q', loc, ts).u64(shares).alpha(sym, 8).u32(price).u64(match).ch(type).done();
}
inline Buf broken(uint16_t loc, uint64_t ts, uint64_t match) {
    return W().head('B', loc, ts).u64(match).done();
}

// BinaryFILE stream builder: 2-byte big-endian length, then the payload.
class Stream {
  public:
    Stream& add(const Buf& m) {
        data_.push_back(static_cast<std::byte>(m.size() >> 8));
        data_.push_back(static_cast<std::byte>(m.size() & 0xff));
        data_.insert(data_.end(), m.begin(), m.end());
        ++n_;
        return *this;
    }
    const Buf& bytes() const { return data_; }
    size_t count() const { return n_; }

  private:
    Buf data_;
    size_t n_ = 0;
};

}  // namespace itch::test
