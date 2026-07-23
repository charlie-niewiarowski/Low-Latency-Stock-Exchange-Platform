//
// Orderbook — autonomous price-time-priority book (see orderbook.hpp).
//
// The add/cancel/modify/match logic here was previously owned by the Engine; it
// now lives entirely inside the book, reached through process(OrderRequest).
//

#include <algorithm>

#include "../include/orderbook.hpp"
#include "lib.hpp"

#if LOGGING
#include <iostream>
#endif

//=============================================================================
// construction
//=============================================================================

Orderbook::Orderbook(OutboundRing& out_ring)
    : pool_(PREALLOCATION_COUNT), out_ring_(out_ring) {
    orders_.reserve(PREALLOCATION_COUNT);
}

//=============================================================================
// process() — the single entry point
//=============================================================================

ProcessResult Orderbook::process(const OrderRequest& req) {
    switch (req.get_message_type()) {
        case MessageType::NEW:
            return { Status::SUCCESS, addOrder(req) };

        case MessageType::CANCEL: {
            const auto it = orders_.find(req.get_order_id());
            if (it == orders_.end() || it->second->client_id_ != req.get_clientId()) {
                return { Status::FAILURE, false };
            }
            cancelOrder(req.get_order_id());
            return { Status::SUCCESS, false };
        }

        case MessageType::MODIFY: {
            const auto it = orders_.find(req.get_order_id());
            if (it == orders_.end()
                    || it->second->client_id_ != req.get_clientId()
                    || it->second->side_ != req.get_side()) {
                return { Status::FAILURE, false };
            }
            return { Status::SUCCESS, modifyOrder(req) };
        }

        default:
            #if LOGGING
            std::cerr << "orderbook: request with unknown type\n";
            #endif
            return { Status::FAILURE, false };
    }
}

//=============================================================================
// prefetch() — warm the structures the next request will touch
//=============================================================================

void Orderbook::prefetch(const InboundMessage& next) const {
    switch (next.message_type) {
        case MessageType::NEW: {
            // The price ladder is a ~2.4 MB sparse array indexed by tick; warm the
            // slot the add will hit. Guard the index (prefetch never faults, but
            // keep the pointer in-bounds).
            if (next.price < MIN_PRICE || next.price > MAX_PRICE) return;
            const size_t idx = next.price - MIN_PRICE;
            prefetch_read(next.side == Side::BID ? &bids_[idx] : &asks_[idx]);
            break;
        }
        case MessageType::CANCEL:
        case MessageType::MODIFY: {
            // std::unordered_map exposes only the bucket index, so this is an
            // early-touch of the bucket's first node rather than a pure async
            // prefetch (see the plan's map-node caveat).
            if (orders_.bucket_count() == 0) return;
            const size_t b = orders_.bucket(next.order_id);
            const auto lit = orders_.begin(b);
            if (lit != orders_.end(b)) prefetch_read(&*lit);
            break;
        }
        default:
            break;
    }
}

//=============================================================================
// state modifications
//=============================================================================

bool Orderbook::addOrder(const OrderRequest& order_request) {
    if (order_request.get_type() == OrderType::MARKET) {
        return matchMarket(order_request);
    }

    const OrderId id = order_request.get_order_id();
    if (orders_.contains(id)) return false;

    Order* order_ptr = pool_.allocate(id, order_request);
    if (!order_ptr) return false;   // pool exhausted — reject the order
    orders_.emplace(id, order_ptr);
    Order& order = *order_ptr;

    if (order.side_ == Side::BID) {
        if (bids_[order.price_ - MIN_PRICE].empty()) ++non_empty_bid_levels_;
        bids_[order.price_ - MIN_PRICE].push(&order);
        if (order.price_ > best_bid_price_) best_bid_price_ = order.price_;
    } else {
        if (asks_[order.price_ - MIN_PRICE].empty()) ++non_empty_ask_levels_;
        asks_[order.price_ - MIN_PRICE].push(&order);
        if (order.price_ < best_ask_price_) best_ask_price_ = order.price_;
    }

    return matchOrders();
}

void Orderbook::cancelOrder(const OrderId id) {
    Order* order = orders_.at(id);
    const auto price = order->price_;

    if (order->side_ == Side::BID) {
        bids_[price - MIN_PRICE].remove(order);
        if (bids_[price - MIN_PRICE].empty()) {
            --non_empty_bid_levels_;
            if (non_empty_bid_levels_ == 0)    best_bid_price_ = MIN_PRICE;
            else if (price == best_bid_price_) update_best_bid();
        }
    } else {
        asks_[price - MIN_PRICE].remove(order);
        if (asks_[price - MIN_PRICE].empty()) {
            --non_empty_ask_levels_;
            if (non_empty_ask_levels_ == 0)    best_ask_price_ = MAX_PRICE;
            else if (price == best_ask_price_) update_best_ask();
        }
    }

    orders_.erase(id);
    pool_.deallocate(order);   // return the slot to the pool for reuse
}

bool Orderbook::modifyOrder(const OrderRequest& order_request) {
    const OrderId id = order_request.get_order_id();
    Order* order = orders_.at(id);

    const auto new_price = order_request.get_price();
    const auto new_quantity = order_request.get_quantity();
    if (new_price != order->price_ || new_quantity != order->initial_quantity_) {
        cancelOrder(id);
        return addOrder(order_request);
    }

    // Same price and quantity: update only timestamps so prev_/next_ links are preserved.
    order->recv_tsc = order_request.get_recv_tsc();
#if DIAGNOSTICS
    order->server_push_tsc = order_request.get_server_push_tsc();
    order->engine_pop_tsc  = order_request.get_engine_in_tsc();
#endif
    if (order_request.get_type() == OrderType::MARKET) {
        return matchMarket(order_request);
    }

    return matchOrders();
}

//=============================================================================
// matching
//=============================================================================

bool Orderbook::matchOrders() {
    bool any_filled = false;

    while (non_empty_bid_levels_ > 0 && non_empty_ask_levels_ > 0) {
        auto& highest_bids = bids_[best_bid_price_ - MIN_PRICE];
        auto& lowest_asks = asks_[best_ask_price_ - MIN_PRICE];

        bool level_matched = false;

        while (!highest_bids.empty() && !lowest_asks.empty()) {
            // get the orders
            auto bid = highest_bids.front();
            auto ask = lowest_asks.front();

            // LIMIT orders only fill when prices cross; MARKET orders fill at any price
            if (best_bid_price_ < best_ask_price_) break;

            level_matched = true;
            any_filled = true;

            // fill the orders
            auto fill_quantity = std::min(bid->remaining_quantity_, ask->remaining_quantity_);

            #if LOGGING
            Trade trade{
                TradeInfo(bid->client_id_, bid->id_, bid->price_, fill_quantity),
                TradeInfo(ask->client_id_, ask->id_, ask->price_, fill_quantity)
            };
            #endif

            OutboundMessage buy_msg{
                .recv_tsc       = bid->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = bid->engine_pop_tsc,
                #endif
                .order_id       = bid->id_,
                .client_id      = bid->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = ask->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = ask->engine_pop_tsc,
                #endif
                .order_id       = ask->id_,
                .client_id      = ask->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            bid->fill(fill_quantity);
            ask->fill(fill_quantity);

            out_ring_.push(buy_msg);
            out_ring_.push(ask_msg);

            // remove orders if filled
            if (bid->is_filled()) {
                highest_bids.pop();
                orders_.erase(bid->id_);
                pool_.deallocate(bid);
            }
            if (ask->is_filled()) {
                lowest_asks.pop();
                orders_.erase(ask->id_);
                pool_.deallocate(ask);
            }

            #if LOGGING
            trades_ring_.push(trade);
            #endif

            // erase empty price levels and break to avoid dangling references
            const bool bid_level_done = highest_bids.empty();
            const bool ask_level_done = lowest_asks.empty();
            if (bid_level_done) {
                --non_empty_bid_levels_;
                if (non_empty_bid_levels_ == 0) best_bid_price_ = MIN_PRICE;
                else update_best_bid();
            }
            if (ask_level_done) {
                --non_empty_ask_levels_;
                if (non_empty_ask_levels_ == 0) best_ask_price_ = MAX_PRICE;
                else update_best_ask();
            }
            if (bid_level_done || ask_level_done) break;
        }

        if (!level_matched) break;
    }

    return any_filled;
}

bool Orderbook::matchMarket(const OrderRequest& order_request) {
    const OrderId id = order_request.get_order_id();
    auto remaining{order_request.get_quantity()};

    if (order_request.get_side() == Side::BID) {
        while (non_empty_ask_levels_ > 0 && remaining > 0) {
            auto& lowest_asks{asks_[best_ask_price_ - MIN_PRICE]};

            if (lowest_asks.empty()) return false;

            auto ask = lowest_asks.front();

            auto fill_quantity = std::min(remaining, ask->remaining_quantity_);

            OutboundMessage market_msg{
                .recv_tsc       = order_request.get_recv_tsc(),
                .engine_pop_tsc = order_request.get_engine_in_tsc(),
                .order_id       = id,
                .client_id      = order_request.get_clientId(),
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = ask->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = ask->engine_pop_tsc,
                #endif
                .order_id       = ask->id_,
                .client_id      = ask->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            remaining -= fill_quantity;
            ask->fill(fill_quantity);

            out_ring_.push(market_msg);
            out_ring_.push(ask_msg);

            #if LOGGING
            Trade match_trade{
                TradeInfo(order_request.get_clientId(), id, best_ask_price_, fill_quantity),
                TradeInfo(ask->client_id_, ask->id_, ask->price_, fill_quantity)
            };
            #endif

            // remove orders if filled
            if (ask->is_filled()) {
                lowest_asks.pop();
                orders_.erase(ask->id_);
                pool_.deallocate(ask);
            }

            #if LOGGING
            trades_ring_.push(match_trade);
            #endif

            if (lowest_asks.empty()) {
                --non_empty_ask_levels_;
                if (non_empty_ask_levels_ == 0) best_ask_price_ = MAX_PRICE;
                else update_best_ask();
            }
        }
    }
    else if (order_request.get_side() == Side::ASK) {
        while (non_empty_bid_levels_ > 0 && remaining > 0) {
            auto& lowest_bids = bids_[best_bid_price_ - MIN_PRICE];

            if (lowest_bids.empty()) return false;


            auto bid = lowest_bids.front();

            auto fill_quantity = std::min(remaining, bid->remaining_quantity_);

            OutboundMessage market_msg{
                .recv_tsc       = order_request.get_recv_tsc(),
                .engine_pop_tsc = order_request.get_engine_in_tsc(),
                .order_id       = id,
                .client_id      = order_request.get_clientId(),
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };
            OutboundMessage ask_msg{
                .recv_tsc       = bid->recv_tsc,
                #if DIAGNOSTICS
                .engine_pop_tsc = bid->engine_pop_tsc,
                #endif
                .order_id       = bid->id_,
                .client_id      = bid->client_id_,
                .message_type   = MessageType::MATCH,
                .status         = Status::SUCCESS
            };

            remaining -= fill_quantity;
            bid->fill(fill_quantity);

            out_ring_.push(market_msg);
            out_ring_.push(ask_msg);

            #if LOGGING
            Trade match_trade{
                TradeInfo(bid->client_id_, bid->id_, bid->price_, fill_quantity),
                TradeInfo(order_request.get_clientId(), id, best_bid_price_, fill_quantity)
            };
            #endif

            // remove orders if filled
            if (bid->is_filled()) {
                lowest_bids.pop();
                orders_.erase(bid->id_);
                pool_.deallocate(bid);
            }

            #if LOGGING
            trades_ring_.push(match_trade);
            #endif

            if (lowest_bids.empty()) {
                --non_empty_bid_levels_;
                if (non_empty_bid_levels_ == 0) best_bid_price_ = MIN_PRICE;
                else update_best_bid();
            }
        }
    }

        return remaining == 0;
}

bool Orderbook::canMatch(const Side side, const Price price) const {
    if (side == Side::BID) {
        if (non_empty_ask_levels_ == 0) return false;
        return price >= best_ask_price_;
    }

    if (non_empty_bid_levels_ == 0) return false;
    return price <= best_bid_price_;
}

void Orderbook::update_best_bid() {
    while (best_bid_price_ > MIN_PRICE && bids_[best_bid_price_ - MIN_PRICE].empty()) {
        --best_bid_price_;
    }
}

void Orderbook::update_best_ask() {
    while (best_ask_price_ < MAX_PRICE && asks_[best_ask_price_ - MIN_PRICE].empty()) {
        ++best_ask_price_;
    }
}
