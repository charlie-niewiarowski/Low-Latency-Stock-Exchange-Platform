//
// Created by charl on 1/17/2026.
//

#include "exchange.hpp"
#include "perf.hpp"

#include <thread>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>

std::atomic<bool> Exchange::stop_{false};

// engine_ is constructed first, and its Orderbook calibrates the TscClock
// (~50ms) before any thread that reads it starts.
Exchange::Exchange() : engine_(*in_ring_, *out_ring_, market_data_ring_, stop_), server_{in_ring_, out_ring_} {}

void Exchange::run() {
    server_.run();
    engine_.run();
    // Every thread is up and named by now: list them all, for perf -t.
    print_threads(std::cerr);

    stop_.wait(false);
    server_.stop(); // the engine's matching thread watches stop_ itself
}

void Exchange::stop(int sig) {
    stop_.store(true);
    stop_.notify_all();
}
