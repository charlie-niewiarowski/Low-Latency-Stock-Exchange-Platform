#include "exchange.hpp"

#include <csignal>
#include <iostream>

int main() {
    Exchange exchange;

    #if DIAGNOSTICS
    std::cerr << "exchange tid: " << gettid() << std::endl;
    #endif

    std::signal(SIGINT, Exchange::stop);
    std::signal(SIGTERM, Exchange::stop);

    exchange.run();

    return 0;
}
