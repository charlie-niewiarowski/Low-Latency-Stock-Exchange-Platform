//
// Created by cniew on 10/18/26.
//

#include "../include/order_entry.hpp"

//=============================================================================
// translate() -- the single entry point
//=============================================================================

Translation OrderEntry::translate(const ClientId client_id, const OuchInboundFrame &msg) {
    switch (msg.message_type) {
        case InboundOuchMessageType::ENTER_ORDER:   return enter_(client_id, msg.enter_order);
        case InboundOuchMessageType::CANCEL_ORDER:  return cancel_(client_id, msg.cancel_order);
        case InboundOuchMessageType::REPLACE_ORDER: return replace_(client_id, msg.replace_order);
        case InboundOuchMessageType::NONE:          return {};
    }
    return {}; // defensive: only reachable via an out-of-range enum value
}

//=============================================================================
// Enter Order
//=============================================================================
//
// A UserRefNum at or below the client's last one is a retransmission and is
// ignored. Anything else consumes the UserRefNum, accepted or rejected.

Translation OrderEntry::enter_(const ClientId client_id, const OuchEnterOrder &msg) {
    ClientState &client = clients_[client_id];
    if (msg.user_ref_num <= client.last_user_ref_num) {
        return {};
    }
    client.last_user_ref_num = msg.user_ref_num;

    const OrderRecord record{
        .client_id                     = client_id,
        .user_ref_num                  = msg.user_ref_num,
        .quantity                      = msg.quantity,
        .price                         = msg.price,
        .symbol                        = msg.symbol,
        .cl_ord_id                     = msg.cl_ord_id,
        .side                          = msg.side,
        .time_in_force                 = msg.time_in_force,
        .display                       = msg.display,
        .capacity                      = msg.capacity,
        .intermarket_sweep_eligibility = msg.intermarket_sweep_eligibility,
        .cross_type                    = msg.cross_type,
    };

    const validation::EnterCheck check = validation::check_enter(msg, band_);
    if (check.reject_reason != 0) {
        return {.note = GatewayNote{.kind = GatewayNote::Kind::REJECTED, .reject_reason = check.reject_reason,
                                    .order_id = 0, .record = record}};
    }

    const OrderId order_id = next_order_id_++;

    return {
        .note = GatewayNote{.kind = GatewayNote::Kind::ORDER_ENTERED, .reject_reason = 0,
                            .order_id = order_id, .record = record},
        .request = EngineRequest{
            .read_begin_tsc  = 0,
            .recv_tsc        = 0,
            .server_push_tsc = 0,
            .order_id        = order_id,
            .client_id       = client_id,
            .client_ref      = msg.user_ref_num,
            .new_client_ref  = 0,
            .price           = check.price,
            .quantity        = msg.quantity,
            .type            = EngineRequestType::NEW,
            .side            = check.side,
            .order_type      = check.order_type,
            .time_in_force   = check.time_in_force,
            .side_given      = false,
            .padding         = {},
        },
    };
}

//=============================================================================
// Cancel Order
//=============================================================================
//
// No ack of its own -- the engine's CANCELED event is the only response.
// A cancel of an order that's unknown or already dead is silently ignored:
// here for a UserRefNum the client hasn't used yet, by the engine otherwise.

Translation OrderEntry::cancel_(const ClientId client_id, const OuchCancelOrder &msg) {
    if (!issued_(client_id, msg.user_ref_num)) {
        return {};
    }
    return {.request = cancel_request_(client_id, msg.user_ref_num, msg.quantity)};
}

//=============================================================================
// Replace Order
//=============================================================================
//
// Per spec (2.2):
//   * unknown original, or a replacement UserRefNum that's already been
//     used: silently ignored, replacement UserRefNum not consumed
//   * invalid replace details: the existing order is canceled instead,
//     replacement UserRefNum not consumed
//   * otherwise: cancel + new under a new OrderId, losing time priority
// An original this gateway can't rule out -- a UserRefNum the client has
// used -- goes to the engine, which ignores it if no such order is live. So
// does the side check (the original's side is the engine's to know). Both
// of those then consume the replacement UserRefNum, a deviation from the
// spec only a client replacing a rejected or mis-sided order can see.

Translation OrderEntry::replace_(const ClientId client_id, const OuchReplaceOrder &msg) {
    if (!issued_(client_id, msg.orig_user_ref_num)) {
        return {};
    }
    ClientState &client = clients_[client_id];
    if (msg.user_ref_num <= client.last_user_ref_num) {
        return {};
    }

    const validation::ReplaceCheck check = validation::check_replace(msg, band_);
    if (!check.valid) {
        return {.request = cancel_request_(client_id, msg.orig_user_ref_num, 0)};
    }
    client.last_user_ref_num = msg.user_ref_num;

    // side, symbol, capacity and cross_type are the original's; the
    // responder fills them in (see OrderRecord).
    const OrderRecord record{
        .client_id                     = client_id,
        .user_ref_num                  = msg.user_ref_num,
        .quantity                      = msg.quantity,
        .price                         = msg.price,
        .symbol                        = {},
        .cl_ord_id                     = msg.cl_ord_id,
        .side                          = 0,
        .time_in_force                 = msg.time_in_force,
        .display                       = msg.display,
        .capacity                      = 0,
        .intermarket_sweep_eligibility = msg.intermarket_sweep_eligibility,
        .cross_type                    = 0,
    };

    const OrderId new_order_id = next_order_id_++;
    return {
        .note = GatewayNote{.kind = GatewayNote::Kind::ORDER_REPLACING, .reject_reason = 0,
                            .order_id = new_order_id, .record = record},
        .request = EngineRequest{
            .read_begin_tsc  = 0,
            .recv_tsc        = 0,
            .server_push_tsc = 0,
            .order_id        = new_order_id,
            .client_id       = client_id,
            .client_ref      = msg.orig_user_ref_num,
            .new_client_ref  = msg.user_ref_num,
            .price           = check.price,
            .quantity        = msg.quantity,
            .type            = EngineRequestType::REPLACE,
            .side            = check.side,
            .order_type      = OrderType::LIMIT,
            .time_in_force   = check.time_in_force,
            .side_given      = check.side_given,
            .padding         = {},
        },
    };
}

//=============================================================================
// helpers
//=============================================================================

bool OrderEntry::issued_(const ClientId client_id, const uint32_t user_ref_num) {
    return user_ref_num != 0 && user_ref_num <= clients_[client_id].last_user_ref_num;
}

EngineRequest OrderEntry::cancel_request_(const ClientId client_id, const uint32_t user_ref_num,
                                          const uint32_t quantity) {
    return EngineRequest{
        .read_begin_tsc  = 0,
        .recv_tsc        = 0,
        .server_push_tsc = 0,
        .order_id        = 0,
        .client_id       = client_id,
        .client_ref      = user_ref_num,
        .new_client_ref  = 0,
        .price           = 0,
        .quantity        = quantity,
        .type            = EngineRequestType::CANCEL,
        .side            = Side::BID, // unused
        .order_type      = OrderType::LIMIT,
        .time_in_force   = TimeInForce::DAY,
        .side_given      = false,
        .padding         = {},
    };
}
