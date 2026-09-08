//
// Created by cniew on 9/2/26.
//

#ifndef MARKET_FEED_H
#define MARKET_FEED_H

/*
The market data feed is going to take its input from a SPSC queue that the matching engine publishes to
These will be arbitrary order objects
Our job is to then serialize these order objects into an update object then publish to a
UDP multicast feed
*/

#include <memory>
#include <instance.h>

class market_feed {
public:
    market_feed() = delete;
    explicit market_feed(InboundRing& in_ring);

    market_feed(const market_feed& other) = delete;
    market_feed& operator=(const market_feed& other) = delete;
    market_feed(market_feed&& other) = delete;
    market_feed& operator=(market_feed&& other) = delete;

    ~market_feed();

    void run();
private:
    InboundRing& in_ring_;
    dpdk::instance runtime;
};

#endif //MARKET_FEED_H
