#include "exchange.hpp"
#include "perf.hpp"

#include <csignal>
#include <iostream>

int main() {
    Exchange exchange;
    // After Exchange is built: the threads DPDK starts while bringing up the
    // EAL would otherwise inherit this name.
    name_thread(pthread_self(), "exchange-main");

    std::signal(SIGINT, Exchange::stop);
    std::signal(SIGTERM, Exchange::stop);

    exchange.run();

    return 0;
}
