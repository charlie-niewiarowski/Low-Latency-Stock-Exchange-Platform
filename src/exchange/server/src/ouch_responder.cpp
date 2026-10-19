//
// Created by cniew on 10/18/26.
//

#include "../include/ouch_responder.hpp"

#include "validation.hpp"

//=============================================================================
// notes
//=============================================================================

std::optional<OuchResponse> OuchResponder::on_note(const GatewayNote &note, const uint64_t timestamp) {
    switch (note.kind) {
        case GatewayNote::Kind::ORDER_ENTERED:
        case GatewayNote::Kind::ORDER_REPLACING:
            // Open shares come from the ACCEPTED/REPLACED event, not the note.
            orders_.insert_or_assign(note.order_id, LiveOrder{.record = note.record, .open = 0});
            return std::nullopt;

        case GatewayNote::Kind::REJECTED: {
            OuchResponse response{.client_id = note.record.client_id, .ouch = {}};
            response.ouch.order_rejected = OuchOrderRejected{
                .timestamp    = timestamp,
                .user_ref_num = note.record.user_ref_num,
                .reason       = note.reject_reason,
                .cl_ord_id    = note.record.cl_ord_id,
            };
            return response;
        }
    }
    return std::nullopt; // defensive: only reachable via an out-of-range enum value
}

//=============================================================================
// events
//=============================================================================

std::optional<OuchResponse> OuchResponder::on_event(const EngineEvent &event, const uint64_t timestamp) {
    const auto it = orders_.find(event.order_id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    LiveOrder &order = it->second;
    const OrderRecord &r = order.record;

    OuchResponse response{.client_id = r.client_id, .ouch = {}};
    switch (event.type) {
        case EngineEventType::ACCEPTED:
        {
            order.open = event.quantity;
            OuchOrderAccepted &a = response.ouch.order_accepted = accepted_defaults_;
            a.timestamp                     = timestamp;
            a.user_ref_num                  = r.user_ref_num;
            a.side                          = r.side;
            a.quantity                      = event.quantity;
            a.symbol                        = r.symbol;
            a.price                         = r.price;
            a.time_in_force                 = r.time_in_force;
            a.display                       = r.display;
            a.order_reference_number        = event.order_id;
            a.capacity                      = r.capacity;
            a.intermarket_sweep_eligibility = r.intermarket_sweep_eligibility;
            a.cross_type                    = r.cross_type;
            a.order_state                   = 'L';
            a.cl_ord_id                     = r.cl_ord_id;
            return response;
        }

        case EngineEventType::REPLACED: {
            // Order Replaced names the original by its UserRefNum, and the
            // replacement inherits what a replace can't change (see
            // OrderRecord). The original is dead from here on.
            const auto orig = orders_.find(event.orig_order_id);
            if (orig == orders_.end()) {
                return std::nullopt;
            }
            const OrderRecord &orig_record = orig->second.record;
            const uint32_t orig_user_ref_num = orig_record.user_ref_num;
            order.record.side       = orig_record.side;
            order.record.symbol     = orig_record.symbol;
            order.record.capacity   = orig_record.capacity;
            order.record.cross_type = orig_record.cross_type;
            orders_.erase(orig); // doesn't invalidate it/order/r (a different element)

            order.open = event.quantity;
            OuchOrderReplaced &u = response.ouch.order_replaced = replaced_defaults_;
            u.timestamp                     = timestamp;
            u.orig_user_ref_num             = orig_user_ref_num;
            u.user_ref_num                  = r.user_ref_num;
            u.side                          = r.side;
            u.quantity                      = event.quantity;
            u.symbol                        = r.symbol;
            u.price                         = r.price;
            u.time_in_force                 = r.time_in_force;
            u.display                       = r.display;
            u.order_reference_number        = event.order_id;
            u.capacity                      = r.capacity;
            u.intermarket_sweep_eligibility = r.intermarket_sweep_eligibility;
            u.cross_type                    = r.cross_type;
            u.order_state                   = 'L';
            u.cl_ord_id                     = r.cl_ord_id;
            return response;
        }

        case EngineEventType::CANCELED:
            response.ouch.order_canceled = OuchOrderCanceled{
                .timestamp        = timestamp,
                .user_ref_num     = r.user_ref_num,
                .decrement_shares = event.quantity,
                .reason           = event.reason,
            };
            reduce_(it, event.quantity);
            return response;

        case EngineEventType::EXECUTED:
            response.ouch.order_executed = OuchOrderExecuted{
                .timestamp       = timestamp,
                .user_ref_num    = r.user_ref_num,
                .executed_shares = event.quantity,
                .execution_price = validation::to_ouch_price(event.price),
                .liquidity_flag  = event.reason,
                .match_number    = event.match_number,
            };
            reduce_(it, event.quantity);
            return response;

        case EngineEventType::REJECTED:
            response.ouch.order_rejected = OuchOrderRejected{
                .timestamp    = timestamp,
                .user_ref_num = r.user_ref_num,
                .reason       = ouch_reject::kProcessingError,
                .cl_ord_id    = r.cl_ord_id,
            };
            orders_.erase(it);
            return response;

        case EngineEventType::REPLACE_IGNORED:
            orders_.erase(it); // the replacement's record, from its note; the client hears nothing
            return std::nullopt;
    }
    return std::nullopt; // defensive: only reachable via an out-of-range enum value
}

void OuchResponder::reduce_(const absl::flat_hash_map<OrderId, LiveOrder>::iterator order, const Quantity shares) {
    LiveOrder &live = order->second;
    live.open = shares < live.open ? live.open - shares : 0;
    if (live.open == 0) {
        orders_.erase(order);
    }
}
