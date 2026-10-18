//
// Created by charl on 11/25/2025.
//

#ifndef UNTITLED_ORDERBOOK_H
#define UNTITLED_ORDERBOOK_H

#include "orderbook.hpp"
#include "ring_buffer.hpp"
#include "communication_proto.hpp"
#include "config.hpp"

#include <atomic>
#include <thread>

// The Engine is a thin driver around an autonomous Orderbook. It owns the
// ring wiring and the matching thread, and hands each EngineRequest straight
// to book_.process() (OrderIds are assigned by the gateway, not here).
class Engine {

public:
    Engine(InboundRing& in_ring, OutboundRing& out_ring, MarketDataRing& market_data_ring,
           std::atomic<bool>& stop);

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
    [[nodiscard]] std::optional<EngineEvent> pop_outbound() { return out_ring_.pop(); }
    #endif // TESTING
private:
    //===== threads ======
    std::jthread matching_thread_;

    //===== the book ======
    Orderbook book_;

    //===== communication with server ======
    InboundRing& in_ring_;
    OutboundRing& out_ring_;

    //===== stop ======
    std::atomic<bool>& stop_;

    //===== thread entry =====
    void handleMatching();

    //===== request routing =====
    void executeRequest(const EngineRequest &req);
};


#endif //UNTITLED_ORDERBOOK_H
