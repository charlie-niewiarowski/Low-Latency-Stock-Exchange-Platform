//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H

#include <atomic>
#include <thread>

#include "orderbook.hpp"
#include "ring_buffer.hpp"
#include "communication_types.hpp"
#include "config.hpp"

// The Engine is now a thin driver around an autonomous Orderbook. It owns the
// inbound/outbound ring wiring, the matching thread, and OrderId assignment; it
// translates each InboundMessage into an OrderRequest and routes it to book_.process().
class Engine {

public:
    Engine(InboundRing& in_ring, OutboundRing& out_ring, std::atomic<bool>& stop);

    Engine(const Engine& other) = delete;
    Engine& operator=(const Engine& rhs) = delete;
    Engine(const Engine&& other) = delete;
    Engine& operator=(const Engine&& rhs) = delete;

    ~Engine() = default;

    void run();

    #if TESTING
    void step();
    size_t order_count()     const { return book_.order_count(); }
    size_t bid_level_count() const { return book_.bid_level_count(); }
    size_t ask_level_count() const { return book_.ask_level_count(); }
    size_t bids_at(Price p)  const { return book_.bids_at(p); }
    size_t asks_at(Price p)  const { return book_.asks_at(p); }
    bool has_order(OrderId id) const { return book_.has_order(id); }
    [[nodiscard]] std::optional<OutboundMessage> pop_outbound() { return out_ring_.pop(); }
    [[nodiscard]] std::optional<Trade> pop_trade() { return book_.pop_trade(); }
    OrderId next_order_id() const { return next_id_; }
    #endif // TESTING
private:
    //===== threads ======
    std::jthread matching_thread_;

    //===== the book ======
    Orderbook book_;

    //===== id assignment ======
    OrderId next_id_{0};

    //===== communication with server ======
    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    //===== stop ======
    std::atomic<bool>& stop_;

    //===== thread entry =====
    void handleMatching();

    //===== request routing =====
    void executeRequest(const InboundMessage &msg);

    #if LOGGING
    std::jthread expose_thread_;
    void exposeTrades();
    #endif
};


#endif //UNTITLED_ORDERBOOK_H
