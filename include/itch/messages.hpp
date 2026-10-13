#pragma once
// Decoded message views and the length table for Nasdaq TotalView-ITCH 5.0.
// Every length below was recomputed from the offset tables in the official spec
// (NQTVITCHspecification.pdf), not copied from memory. See docs/design.md.
#include <array>
#include <cstdint>

namespace itch {

// Price(4): integer with 4 implied decimals. Spec max is 200,000.0000 = 2,000,000,000
// (0x77359400), which fits in uint32. Never converted to floating point anywhere in the book.
using Price = uint32_t;
using Locate = uint16_t;
using OrderRef = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

namespace msg {
constexpr char kSystemEvent = 'S', kStockDirectory = 'R', kTradingAction = 'H', kRegSho = 'Y',
               kMpPosition = 'L', kMwcbDecline = 'V', kMwcbStatus = 'W', kIpoQuoting = 'K',
               kLuld = 'J', kOperationalHalt = 'h', kAdd = 'A', kAddMpid = 'F', kExecuted = 'E',
               kExecutedPrice = 'C', kCancel = 'X', kDelete = 'D', kReplace = 'U', kTrade = 'P',
               kCross = 'Q', kBroken = 'B', kNoii = 'I', kRetailInterest = 'N',
               kDirectListing = 'O';
}  // namespace msg

// Total message length in bytes (payload only, excluding the 2-byte BinaryFILE frame length).
// 0 = type not defined by ITCH 5.0.
constexpr std::array<uint8_t, 256> make_length_table() {
    std::array<uint8_t, 256> t{};
    auto set = [&](char c, uint8_t n) { t[static_cast<uint8_t>(c)] = n; };
    set('S', 12);
    set('R', 39);
    set('H', 25);
    set('Y', 20);
    set('L', 26);
    set('V', 35);
    set('W', 12);
    set('K', 28);
    set('J', 35);
    set('h', 21);
    set('A', 36);
    set('F', 40);
    set('E', 31);
    set('C', 36);
    set('X', 23);
    set('D', 19);
    set('U', 35);
    set('P', 44);
    set('Q', 40);
    set('B', 19);
    set('I', 50);
    set('N', 20);
    set('O', 48);
    return t;
}
inline constexpr std::array<uint8_t, 256> kMsgLength = make_length_table();

struct Header {
    Locate locate;
    uint16_t tracking;
    uint64_t ts;  // ns since midnight
};

struct AddOrder {
    Header h;
    OrderRef ref;
    Side side;
    uint32_t shares;
    char stock[8];
    Price price;
    bool has_mpid;
    char mpid[4];
};
struct OrderExecuted {
    Header h;
    OrderRef ref;
    uint32_t shares;
    uint64_t match;
};
struct OrderExecutedPrice {
    Header h;
    OrderRef ref;
    uint32_t shares;
    uint64_t match;
    bool printable;
    Price price;
};
struct OrderCancel {
    Header h;
    OrderRef ref;
    uint32_t shares;
};
struct OrderDelete {
    Header h;
    OrderRef ref;
};
struct OrderReplace {
    Header h;
    OrderRef old_ref, new_ref;
    uint32_t shares;
    Price price;
};
struct Trade {  // non-cross, executes a non-displayed order; no book effect
    Header h;
    OrderRef ref;  // always 0 since Dec 2010
    Side side;     // always Buy since Jul 2014
    uint32_t shares;
    char stock[8];
    Price price;
    uint64_t match;
};
struct CrossTrade {
    Header h;
    uint64_t shares;
    char stock[8];
    Price price;
    uint64_t match;
    char cross_type;
};
struct BrokenTrade {
    Header h;
    uint64_t match;
};
struct StockDirectory {
    Header h;
    char stock[8];
    char market_category;
    char financial_status;
};
struct SystemEvent {
    Header h;
    char code;
};
struct TradingAction {
    Header h;
    char stock[8];
    char state;
};

}  // namespace itch
