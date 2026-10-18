//
// Created by cniew on 10/18/26.
//
// OuchResponder -- engine events (and gateway notes) back into OUCH.
//
// The outbound half of OrderEntry: it keeps the OrderRecord of every live
// order (from the inbound side's GatewayNotes) and turns each EngineEvent
// into the OUCH message for that order's owner, echoing the fields the
// engine never saw. Like OrderEntry it never touches the network or the
// rings; Server drives it from the outbound thread and is its only user.
//
// A record lives from its note until the engine reports the order dead --
// fully executed, canceled to zero, replaced, or rejected -- which it tracks
// by following the open share count the events imply.
//

#ifndef OUCH_RESPONDER_H
#define OUCH_RESPONDER_H

#include "communication_proto.hpp"
#include "order_entry.hpp"
#include "ouch_proto.hpp"

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <optional>

// One OUCH message and the client it goes to.
struct OuchResponse {
    ClientId          client_id;
    OuchOutboundFrame ouch;
};

class OuchResponder {
public:
    OuchResponder() = default;

    OuchResponder(const OuchResponder &) = delete;
    OuchResponder &operator=(const OuchResponder &) = delete;

    // Records a new/replacement order, or answers a gateway-level reject.
    // timestamp is OUCH time (ns since midnight) for any response.
    std::optional<OuchResponse> on_note(const GatewayNote &note, uint64_t timestamp);

    // The OUCH message for event's order owner. nullopt only if the event
    // names an order this responder has no record of (which the note-before-
    // request ordering rules out).
    std::optional<OuchResponse> on_event(const EngineEvent &event, uint64_t timestamp);

    [[nodiscard]] size_t live_orders() const { return orders_.size(); }

private:
    struct LiveOrder {
        OrderRecord record;
        Quantity    open = 0; // shares still on the book, per the events so far
    };

    // Takes shares off order's open count; erases it once nothing is left.
    void reduce_(absl::flat_hash_map<OrderId, LiveOrder>::iterator order, Quantity shares);

    absl::flat_hash_map<OrderId, LiveOrder> orders_;
};

#endif // OUCH_RESPONDER_H
