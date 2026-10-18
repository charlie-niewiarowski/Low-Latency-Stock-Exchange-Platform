//
// Created by cniew on 5/16/26.
//

#ifndef COMMUNICATION_PROTO_H
#define COMMUNICATION_PROTO_H

#include "order_types.hpp"
#include "ouch_proto.hpp"

#include <cstdint>

//=============================================================================
// Inbound/Outbound communication between components
//=============================================================================
// Communication layer between components: what the gateway (Server) and the
// matching engine actually pass each other across the ring buffer. Each of
// these is exchange-side metadata (nothing a client ever sees, none of it
// OUCH) plus one full OuchInboundFrame/OuchOutboundFrame -- so "what OUCH
// data does the engine get" is answered once, by whichever variant is live
// in that frame, rather than by hand-picking a subset of fields up front and
// guessing what a future rewrite might need. order_id/client_id sit outside
// the frame because neither is ever on the OUCH wire itself: user_ref_num
// (inside the frame) is the client's own reference, not the engine's
// OrderId, and client identity comes from the session, not any field.

struct InboundOuch { // gateway -> engine
    Timestamp read_begin_tsc;
    Timestamp recv_tsc;
    Timestamp server_push_tsc;

    OrderId  order_id;    // junk for NEW; resolved by the gateway for CANCEL/REPLACE
    ClientId client_id;   // from the TCP session, never from the wire message itself

    OuchInboundFrame ouch; // ouch.message_type is this message's own discriminant
};

struct OutboundOuch { // engine -> gateway
    Timestamp read_begin_tsc;
    Timestamp recv_tsc;
    Timestamp server_push_tsc;
    Timestamp engine_pop_tsc;
    Timestamp engine_push_tsc;

    OrderId  order_id;
    ClientId client_id;

    OuchOutboundFrame ouch;
};

#endif //COMMUNICATION_PROTO_H
