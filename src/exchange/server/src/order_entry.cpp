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
    orders_.emplace(key_(client_id, msg.user_ref_num), LiveOrder{order_id, record});

    return {
        .note = GatewayNote{.kind = GatewayNote::Kind::ORDER_ENTERED, .reject_reason = 0,
                            .order_id = order_id, .record = record},
        .request = EngineRequest{
            .read_begin_tsc  = 0,
            .recv_tsc        = 0,
            .server_push_tsc = 0,
            .order_id        = order_id,
            .new_order_id    = 0,
            .client_id       = client_id,
            .price           = check.price,
            .quantity        = msg.quantity,
            .type            = EngineRequestType::NEW,
            .side            = check.side,
            .order_type      = check.order_type,
            .time_in_force   = check.time_in_force,
        },
    };
}

//=============================================================================
// Cancel Order
//=============================================================================
//
// No ack of its own -- the engine's CANCELED event is the only response --
// and an unknown UserRefNum is silently ignored, as is cancelling an order
// that's already dead (the engine's call, since only it knows).

Translation OrderEntry::cancel_(const ClientId client_id, const OuchCancelOrder &msg) const {
    const auto it = orders_.find(key_(client_id, msg.user_ref_num));
    if (it == orders_.end()) {
        return {};
    }

    const LiveOrder &order = it->second;
    return {.request = EngineRequest{
        .read_begin_tsc  = 0,
        .recv_tsc        = 0,
        .server_push_tsc = 0,
        .order_id        = order.order_id,
        .new_order_id    = 0,
        .client_id       = client_id,
        .price           = 0,
        .quantity        = msg.quantity,
        .type            = EngineRequestType::CANCEL,
        .side            = *validation::to_engine_side(order.record.side),
        .order_type      = OrderType::LIMIT,
        .time_in_force   = TimeInForce::DAY,
    }};
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
// A replace of an original that's known here but already dead in the book
// is the engine's to ignore.

Translation OrderEntry::replace_(const ClientId client_id, const OuchReplaceOrder &msg) {
    const auto orig = orders_.find(key_(client_id, msg.orig_user_ref_num));
    if (orig == orders_.end()) {
        return {};
    }

    ClientState &client = clients_[client_id];
    if (msg.user_ref_num <= client.last_user_ref_num) {
        return {};
    }

    const OrderId orig_order_id = orig->second.order_id;
    const OrderRecord &orig_record = orig->second.record;
    const Side side = *validation::to_engine_side(orig_record.side);

    const validation::ReplaceCheck check = validation::check_replace(msg, orig_record.side, band_);
    if (!check.valid) {
        return {.request = EngineRequest{
            .read_begin_tsc  = 0,
            .recv_tsc        = 0,
            .server_push_tsc = 0,
            .order_id        = orig_order_id,
            .new_order_id    = 0,
            .client_id       = client_id,
            .price           = 0,
            .quantity        = 0,
            .type            = EngineRequestType::CANCEL,
            .side            = side,
            .order_type      = OrderType::LIMIT,
            .time_in_force   = TimeInForce::DAY,
        }};
    }
    client.last_user_ref_num = msg.user_ref_num;

    OrderRecord record = orig_record;
    record.user_ref_num                  = msg.user_ref_num;
    record.quantity                      = msg.quantity;
    record.price                         = msg.price;
    record.cl_ord_id                     = msg.cl_ord_id;
    record.time_in_force                 = msg.time_in_force;
    record.display                       = msg.display;
    record.intermarket_sweep_eligibility = msg.intermarket_sweep_eligibility;

    const OrderId new_order_id = next_order_id_++;
    // emplace may rehash and invalidate orig/orig_record -- nothing below uses them.
    orders_.emplace(key_(client_id, msg.user_ref_num), LiveOrder{new_order_id, record});

    return {
        .note = GatewayNote{.kind = GatewayNote::Kind::ORDER_REPLACING, .reject_reason = 0,
                            .order_id = new_order_id, .record = record},
        .request = EngineRequest{
            .read_begin_tsc  = 0,
            .recv_tsc        = 0,
            .server_push_tsc = 0,
            .order_id        = orig_order_id,
            .new_order_id    = new_order_id,
            .client_id       = client_id,
            .price           = check.price,
            .quantity        = msg.quantity,
            .type            = EngineRequestType::REPLACE,
            .side            = side,
            .order_type      = OrderType::LIMIT,
            .time_in_force   = check.time_in_force,
        },
    };
}
