//
// Created by charl on 1/17/2026.
//

#ifndef UNTITLED_ORDERBOOK_HPP
#define UNTITLED_ORDERBOOK_HPP

#include "../engine/include/engine.hpp"
#include "../server/include/server.hpp"
#include "config.hpp"

class Exchange {
public:
    Exchange();
    void run();
    static void stop(int sig);
private:
    // Declared (so constructed) before, and destroyed after, the threads
    // that use them. Server shares ownership of its two.
    std::shared_ptr<InboundRing> in_ring_ = std::make_shared<InboundRing>(COMMUNICATION_RING_COUNT);
    std::shared_ptr<OutboundRing> out_ring_ = std::make_shared<OutboundRing>(COMMUNICATION_RING_COUNT);
    // Engine -> market data. Nothing reads it until the dissemination
    // component exists; the engine drops on full (see MarketDataRing).
    MarketDataRing market_data_ring_{COMMUNICATION_RING_COUNT};

    Engine engine_;
    Server server_;
    static std::atomic<bool> stop_;
};


#endif //UNTITLED_ORDERBOOK_HPP