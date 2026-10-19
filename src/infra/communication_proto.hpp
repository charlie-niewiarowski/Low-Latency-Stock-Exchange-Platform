//
// Created by cniew on 5/16/26.
//

#ifndef COMMUNICATION_PROTO_H
#define COMMUNICATION_PROTO_H

#include "order_types.hpp"

#include <cstdint>

//=============================================================================
// Inbound/Outbound communication between components
//=============================================================================
// What the gateway (Server) and the matching engine pass each other across
// the SPSC rings. These are engine-domain messages, not OUCH: the server
// does every protocol-level job before a request reaches the engine
// (validation, OUCH 4-decimal price -> engine cents, market-order sentinel
// detection, (client, UserRefNum) -> OrderId resolution) and turns every
// engine event back into OUCH afterwards, echoing the OUCH-only fields
// (ClOrdID, capacity, display, ...) it kept from the original request. The
// engine never sees a wire format, and each message stays about one
// cache line.
//
// Quantities follow Nasdaq OUCH semantics, which the engine owns because it
// is the only component that knows how much of an order chain has executed:
//   NEW     -- shares to enter
//   CANCEL  -- new intended order size, *inclusive* of shares already
//              executed on the chain; 0 cancels everything still open.
//              Keeps time priority.
//   REPLACE -- total shares liable for the whole chain, inclusive of shares
//              already executed. Always loses time priority (it's a cancel
//              plus a new order under new_order_id).

enum class EngineRequestType : uint8_t { NEW = 0, CANCEL = 1, REPLACE = 2 };

struct EngineRequest { // gateway -> engine
    Timestamp read_begin_tsc;   // 8 bytes  @ 0
    Timestamp recv_tsc;         // 8 bytes  @ 8
    Timestamp server_push_tsc;  // 8 bytes  @ 16

    OrderId order_id;           // 8 bytes  @ 24  |  NEW: id to assign; CANCEL/REPLACE: existing order
    OrderId new_order_id;       // 8 bytes  @ 32  |  REPLACE only: id of the replacement order
    ClientId client_id;         // 4 bytes  @ 40
    Price price;                // 4 bytes  @ 44  |  cents; ignored for MARKET and CANCEL
    Quantity quantity;          // 4 bytes  @ 48  |  see the semantics above
    EngineRequestType type;     // 1 byte   @ 52
    Side side;                  // 1 byte   @ 53  |  NEW only; a replace keeps its order's side
    OrderType order_type;       // 1 byte   @ 54
    TimeInForce time_in_force;  // 1 byte   @ 55
}; // 56 bytes
static_assert(sizeof(EngineRequest) == 56);

// Every event is about one order and goes to that order's owner. A request
// can produce several (e.g. an aggressive NEW: ACCEPTED, then one EXECUTED
// per fill, then CANCELED for an IOC remainder), and the counterparty of
// each fill gets its own EXECUTED; the first one is flagged answers_request.
// Requests the Nasdaq spec says to ignore silently (cancel of a dead order,
// replace of a dead order) produce no client-visible event -- except that a
// replace that doesn't go ahead produces REPLACE_IGNORED for its replacement
// (order_id = the replacement's id): the gateway already holds that order's
// record from its note, and this is how it learns to drop it. No OUCH message
// is sent for it.
enum class EngineEventType : uint8_t {
    ACCEPTED = 0, REPLACED = 1, CANCELED = 2, EXECUTED = 3, REJECTED = 4, REPLACE_IGNORED = 5,
};

// EngineEvent::reason values.
namespace engine_reason {
    // CANCELED -- OUCH Appendix B cancel reasons the engine can produce.
    inline constexpr char kUserRequested    = 'U';
    inline constexpr char kImmediateOrCancel = 'I';
    // EXECUTED -- OUCH Appendix E liquidity flags.
    inline constexpr char kAddedLiquidity   = 'A';
    inline constexpr char kRemovedLiquidity = 'R';
    // REJECTED -- engine-side failures (protocol-level rejects never reach
    // the engine; the server answers those itself).
    inline constexpr char kBookFull         = 'F';
} // namespace engine_reason

struct EngineEvent { // engine -> gateway
    Timestamp read_begin_tsc;   // 8 bytes  @ 0
    Timestamp recv_tsc;         // 8 bytes  @ 8
    Timestamp server_push_tsc;  // 8 bytes  @ 16
    Timestamp engine_pop_tsc;   // 8 bytes  @ 24
    Timestamp engine_push_tsc;  // 8 bytes  @ 32

    OrderId order_id;           // 8 bytes  @ 40  |  the order this event is about (REPLACED: the replacement)
    OrderId orig_order_id;      // 8 bytes  @ 48  |  REPLACED only: the order that was replaced
    uint64_t match_number;      // 8 bytes  @ 56  |  EXECUTED only; shared by both sides of a fill
    ClientId client_id;         // 4 bytes  @ 64  |  owner of order_id
    Price price;                // 4 bytes  @ 68  |  cents. ACCEPTED/REPLACED: order price; EXECUTED: fill price
    Quantity quantity;          // 4 bytes  @ 72  |  ACCEPTED/REPLACED: shares open; CANCELED: shares
                                //                   decremented (incremental); EXECUTED: shares filled
    EngineEventType type;       // 1 byte   @ 76
    char reason;                // 1 byte   @ 77  |  see engine_reason
    bool answers_request;       // 1 byte   @ 78  |  the first event its request produced -- the
                                //                   one that answers it (see latency.hpp)
    uint8_t padding[1];         // 1 byte   @ 79
}; // 80 bytes
static_assert(sizeof(EngineEvent) == 80);

#endif //COMMUNICATION_PROTO_H
