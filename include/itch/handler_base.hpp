#pragma once
// Handlers are passed to the parser as a template parameter (static dispatch, no virtuals).
// Derive from HandlerBase to get no-op defaults, then hide the callbacks you care about.
#include <cstdint>

#include "itch/messages.hpp"

namespace itch {

struct HandlerBase {
    void begin(uint8_t /*type*/) {}  // every frame, including malformed ones
    void end() {}                    // after the frame has been fully handled
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_trading_action(const TradingAction&) {}
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_price(const OrderExecutedPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_cross(const CrossTrade&) {}
    void on_broken(const BrokenTrade&) {}
    void on_other(uint8_t /*type*/) {}      // valid ITCH type we only count
    void on_unknown(uint8_t /*type*/) {}    // type byte not in ITCH 5.0
    void on_malformed(uint8_t /*type*/) {}  // known type, wrong length or bad field
};

}  // namespace itch
