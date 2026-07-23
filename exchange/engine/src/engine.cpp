//
// Created by charl on 11/25/2025.
//

#include <immintrin.h>
#include <iostream>
#include <thread>

#include "../include/engine.hpp"
#include "lib.hpp"

//=============================================================================
// Forward-declared free forms
//=============================================================================

#if LOGGING
void write_to_console(const Trade &trade);
#endif

//=============================================================================
// Special Member Functions
//=============================================================================
Engine::Engine(InboundRing& in_ring, OutboundRing& out_ring, std::atomic<bool>& stop)
    : book_(out_ring), in_ring_(in_ring), out_ring_(out_ring), stop_(stop) {}

//=============================================================================
// run()
//=============================================================================
void Engine::run() {
    matching_thread_ = std::jthread([&]() {
        #if DIAGNOSTICS
        std::cerr << "matching tid: " << gettid() << std::endl;
        #endif

        pin_to_core(MATCHING_CORE);
        handleMatching();
    });

    #if LOGGING
         expose_thread_ = std::jthread([&]() {
           exposeTrades();
        });
    #endif
}

//=============================================================================
// step()  — test interface, processes all pending inbound messages once
//=============================================================================
#if TESTING
void Engine::step() {
    InboundMessage msg{};
    while (in_ring_.pop(msg)) {
        executeRequest(msg);
    }
}
#endif

//=============================================================================
// run() Helper Suite
//=============================================================================
void Engine::handleMatching() {
    // 1-ahead pipeline: grab the next request and prefetch the book structures it
    // will touch while we process the current one, hiding the price-ladder /
    // orders_ miss behind the current request's work.
    InboundMessage cur{}, nxt{};
    bool have = false;

    while (!stop_.load(std::memory_order_relaxed)) {
        if (!have) {
            have = in_ring_.pop(cur);
            if (!have) continue;
        }

        const bool have_next = in_ring_.pop(nxt);
        if (have_next) book_.prefetch(nxt);

        executeRequest(cur);

        if (have_next) { cur = nxt; have = true; }
        else            have = false;
    }
}

#if LOGGING
void Engine::exposeTrades() {
    Trade trade{};

    while (!stop_.load(std::memory_order_relaxed)) {
        while (book_.pop_trade(trade)) {
            write_to_console(trade);
        }
    }
}
#endif

//=============================================================================
// request routing
//=============================================================================
//
// The Engine assigns OrderIds for NEW orders, packages the inbound message into
// an OrderRequest, and routes it to the autonomous Orderbook. The book performs
// the add/cancel/modify/match and pushes any MATCH fills onto the outbound ring
// itself; it returns a ProcessResult telling us whether (and how) to ack.
void Engine::executeRequest(const InboundMessage &msg) {
#if DIAGNOSTICS
    const Timestamp engine_pop_tsc = __rdtsc(); // t3
#endif

    // For NEW orders assign the id now so it can be returned in the response.
    const OrderId assigned_id = (msg.message_type == MessageType::NEW) ? next_id_++ : msg.order_id;

    const OrderRequest req{
        msg.message_type,
        assigned_id,
        msg.client_id,
        msg.side,
        msg.order_type,
        msg.price,
        msg.quantity,
#if DIAGNOSTICS
        msg.recv_tsc,
        msg.server_push,
        engine_pop_tsc,
#endif
    };

    const ProcessResult res = book_.process(req);

    if (!res.suppress_ack) {
        OutboundMessage outbound_msg{
            .read_begin_tsc  = msg.read_begin_tsc,
#if DIAGNOSTICS
            .recv_tsc        = msg.recv_tsc,
            .server_push_tsc = msg.server_push,
            .engine_pop_tsc  = engine_pop_tsc,
#endif
            .order_id        = assigned_id,
            .client_id       = msg.client_id,
            .message_type    = msg.message_type,
            .status          = res.status
        };
#if DIAGNOSTICS
        outbound_msg.engine_push_tsc = __rdtsc(); // t4
#endif
        out_ring_.push(outbound_msg);
    }
}

//=============================================================================
// Forward-declared free form implementations
//=============================================================================

#if LOGGING
void write_to_console(const Trade &trade) {
    const TradeInfo& bid{ trade.get_bid_info() };
    const TradeInfo& ask{ trade.get_ask_info() };

    std::cout << "bid -> " << "client_id: " << bid.client_id_
                           << " order_id: " << bid.order_id
                           << " price: " << bid.price
                           << " quantity: " << bid.quantity << "\n";
    std::cout << "ask -> " << "client_id: " << ask.client_id_
                           << " order_id: " << ask.order_id
                           << " price: " << ask.price
                           << " quantity: " << ask.quantity << "\n\n";
}
#endif
