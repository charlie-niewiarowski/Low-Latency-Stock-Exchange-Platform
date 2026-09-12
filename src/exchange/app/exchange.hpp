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
    InboundRing in_ring_{COMMUNICATION_RING_COUNT};
    OutboundRing out_ring_{COMMUNICATION_RING_COUNT};

    Engine engine_;
    Server server_;
    static std::atomic<bool> stop_;
};


#endif //UNTITLED_ORDERBOOK_HPP