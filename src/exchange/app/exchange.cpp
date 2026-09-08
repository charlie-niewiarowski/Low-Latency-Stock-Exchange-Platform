//
// Created by charl on 1/17/2026.
//

#include "exchange.hpp"

#include <thread>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>

std::atomic<bool> Exchange::stop_{false};

Exchange::Exchange() : engine_(in_ring_, out_ring_, stop_), server_{in_ring_, out_ring_, stop_} {}

void Exchange::run() {
    server_.run();
    engine_.run();

    stop_.wait(false);
}

void Exchange::stop(int sig) {
    stop_.store(true);
    stop_.notify_all();
}
