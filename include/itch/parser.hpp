#pragma once
// Zero-copy decode + dispatch, and the BinaryFILE framer.
//
// BinaryFILE framing (Nasdaq binaryfile.pdf): each message is preceded by a 2-byte big-endian
// length that counts the payload only. The parser validates the exact length for the type
// before touching any field, so a hostile or truncated buffer can never be read out of bounds.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "itch/endian.hpp"
#include "itch/handler_base.hpp"
#include "itch/messages.hpp"

namespace itch {

enum class ParseStatus : uint8_t { Ok, Unknown, BadLength, BadField };

namespace detail {
inline Header header(const std::byte* p) {
    return Header{be16(p + 1), be16(p + 3), be48(p + 5)};
}
inline void copy8(char* dst, const std::byte* p) {
    std::memcpy(dst, p, 8);
}
inline bool side_of(std::byte c, Side& out) {
    if (c == std::byte{'B'}) {
        out = Side::Buy;
        return true;
    }
    if (c == std::byte{'S'}) {
        out = Side::Sell;
        return true;
    }
    return false;
}
}  // namespace detail

template <class H>
inline ParseStatus parse_message(Bytes m, H& h) {
    if (m.empty()) {
        h.begin(0);
        h.on_malformed(0);
        h.end();
        return ParseStatus::BadLength;
    }
    const std::byte* p = m.data();
    const uint8_t type = be8(p);
    h.begin(type);
    const uint8_t want = kMsgLength[type];
    ParseStatus st = ParseStatus::Ok;
    if (want == 0) {
        h.on_unknown(type);
        st = ParseStatus::Unknown;
    } else if (m.size() != want) {
        h.on_malformed(type);
        st = ParseStatus::BadLength;
    } else {
        using namespace detail;
        switch (static_cast<char>(type)) {
            case msg::kAdd:
            case msg::kAddMpid: {
                AddOrder a{};
                a.h = header(p);
                a.ref = be64(p + 11);
                if (!side_of(p[19], a.side)) {
                    h.on_malformed(type);
                    st = ParseStatus::BadField;
                    break;
                }
                a.shares = be32(p + 20);
                copy8(a.stock, p + 24);
                a.price = be32(p + 32);
                a.has_mpid = type == static_cast<uint8_t>(msg::kAddMpid);
                if (a.has_mpid) std::memcpy(a.mpid, p + 36, 4);
                h.on_add(a);
                break;
            }
            case msg::kExecuted: {
                OrderExecuted e{header(p), be64(p + 11), be32(p + 19), be64(p + 23)};
                h.on_executed(e);
                break;
            }
            case msg::kExecutedPrice: {
                const std::byte pr = p[31];
                if (pr != std::byte{'Y'} && pr != std::byte{'N'}) {
                    h.on_malformed(type);
                    st = ParseStatus::BadField;
                    break;
                }
                OrderExecutedPrice e{header(p),    be64(p + 11),         be32(p + 19),
                                     be64(p + 23), pr == std::byte{'Y'}, be32(p + 32)};
                h.on_executed_price(e);
                break;
            }
            case msg::kCancel: {
                OrderCancel c{header(p), be64(p + 11), be32(p + 19)};
                h.on_cancel(c);
                break;
            }
            case msg::kDelete: {
                OrderDelete d{header(p), be64(p + 11)};
                h.on_delete(d);
                break;
            }
            case msg::kReplace: {
                OrderReplace r{header(p), be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)};
                h.on_replace(r);
                break;
            }
            case msg::kTrade: {
                Trade t{};
                t.h = header(p);
                t.ref = be64(p + 11);
                if (!side_of(p[19], t.side)) {
                    h.on_malformed(type);
                    st = ParseStatus::BadField;
                    break;
                }
                t.shares = be32(p + 20);
                copy8(t.stock, p + 24);
                t.price = be32(p + 32);
                t.match = be64(p + 36);
                h.on_trade(t);
                break;
            }
            case msg::kCross: {
                CrossTrade c{};
                c.h = header(p);
                c.shares = be64(p + 11);
                copy8(c.stock, p + 19);
                c.price = be32(p + 27);
                c.match = be64(p + 31);
                c.cross_type = static_cast<char>(p[39]);
                h.on_cross(c);
                break;
            }
            case msg::kBroken: {
                h.on_broken(BrokenTrade{header(p), be64(p + 11)});
                break;
            }
            case msg::kStockDirectory: {
                StockDirectory d{};
                d.h = header(p);
                copy8(d.stock, p + 11);
                d.market_category = static_cast<char>(p[19]);
                d.financial_status = static_cast<char>(p[20]);
                h.on_stock_directory(d);
                break;
            }
            case msg::kSystemEvent: {
                h.on_system_event(SystemEvent{header(p), static_cast<char>(p[11])});
                break;
            }
            case msg::kTradingAction: {
                TradingAction t{};
                t.h = header(p);
                copy8(t.stock, p + 11);
                t.state = static_cast<char>(p[19]);
                h.on_trading_action(t);
                break;
            }
            default:
                h.on_other(type);
                break;
        }
    }
    h.end();
    return st;
}

// Iterates BinaryFILE frames over a buffer. Stops (returns false) at a partial trailing frame,
// leaving `consumed()` at the start of it so a streaming caller can carry the remainder over.
class FrameCursor {
  public:
    explicit FrameCursor(Bytes data) : data_(data) {}
    bool next(Bytes& msg) {
        for (;;) {
            if (data_.size() - pos_ < 2) return false;
            const size_t len = be16(data_.data() + pos_);
            if (data_.size() - pos_ - 2 < len) return false;
            const size_t start = pos_ + 2;
            pos_ = start + len;
            if (len == 0) {
                ++zero_len_;
                continue;
            }
            msg = data_.subspan(start, len);
            return true;
        }
    }
    size_t consumed() const { return pos_; }
    uint64_t zero_length_frames() const { return zero_len_; }

  private:
    Bytes data_;
    size_t pos_ = 0;
    uint64_t zero_len_ = 0;
};

struct FrameStats {
    uint64_t messages = 0;
    uint64_t zero_length_frames = 0;
    size_t trailing_bytes = 0;  // partial frame left at the end (0 for a clean file)
};

// Process every complete frame in `data`. Returns bytes consumed.
template <class H>
inline size_t consume_frames(Bytes data, H& h, FrameStats& stats) {
    FrameCursor cur(data);
    Bytes m;
    while (cur.next(m)) {
        parse_message(m, h);
        ++stats.messages;
    }
    stats.zero_length_frames += cur.zero_length_frames();
    return cur.consumed();
}

}  // namespace itch
