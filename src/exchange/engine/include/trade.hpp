//
// Created by charl on 1/8/2026.
//

#ifndef UNTITLED_TRADE_H
#define UNTITLED_TRADE_H

#include <vector>

#include "order_types.hpp"

struct TradeInfo { // used to output results
    ClientId client_id_;
    OrderId order_id;
    Price price;
    Quantity quantity;
};

class Trade {
private:
    TradeInfo bid_info_;
    TradeInfo ask_info_;

public:
    Trade() = default;
    Trade(const TradeInfo& bid_info, const TradeInfo& ask_info) : bid_info_(bid_info), ask_info_(ask_info) {}

    const TradeInfo& get_bid_info() const { return bid_info_; }
    const TradeInfo& get_ask_info() const { return ask_info_; }
};

using Trades = std::vector<Trade>;

#endif //UNTITLED_TRADE_H