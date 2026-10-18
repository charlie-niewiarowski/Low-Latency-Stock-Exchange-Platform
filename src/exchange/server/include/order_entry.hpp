//
// Created by cniew on 10/18/26.
//
// OrderEntry -- OUCH order entry, translated into engine requests.
//
// Session-agnostic: it's handed one decoded OUCH message plus the ClientId
// of the session it arrived on, and owns everything protocol-level about
// order entry -- UserRefNum sequencing, validation, OUCH -> engine field
// conversion, and (ClientId, UserRefNum) -> OrderId assignment. It never
// touches the network or the rings, so it can be tested directly; Server
// drives it from the inbound thread and is its only user.
//
// OrderIds are assigned here, at Enter/Replace time, so a Cancel/Replace
// that arrives before the engine has even seen the original order still
// resolves -- nothing waits on an engine round trip.
//

#ifndef ORDER_ENTRY_H
#define ORDER_ENTRY_H

#include "communication_proto.hpp"
#include "order_types.hpp"
#include "ouch_proto.hpp"
#include "validation.hpp"

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <optional>

// The OUCH-side facts about an order that acks have to echo back (Accepted
// and Replaced repeat the request's fields) but the engine never needs.
struct OrderRecord {
    ClientId        client_id;
    uint32_t        user_ref_num;
    uint32_t        quantity;     // as entered/replaced, OUCH semantics
    uint64_t        price;        // OUCH 4-decimal price, as entered/replaced
    OuchStockSymbol symbol;
    OuchClOrdId     cl_ord_id;
    char            side;
    char            time_in_force;
    char            display;
    char            capacity;
    char            intermarket_sweep_eligibility;
    char            cross_type;
};

// Inbound -> outbound side channel. The outbound thread is the only one that
// sends to clients, and it needs OrderRecords to build acks, so every new
// order's record (and every reject the gateway answers itself) travels to it
// on its own SPSC ring. A note is always pushed before the request it
// belongs to, so it's already visible by the time the engine's resulting
// event reaches the outbound thread.
struct GatewayNote {
    enum class Kind : uint8_t {
        ORDER_ENTERED,    // order_id is a new order; record is what to echo on ACCEPTED
        ORDER_REPLACING,  // order_id is a replacement; record is what to echo on REPLACED
        REJECTED,         // gateway-level reject; record identifies the rejected message
    };

    Kind        kind;
    uint16_t    reject_reason; // REJECTED only: an ouch_reject code
    OrderId     order_id;      // unused for REJECTED
    OrderRecord record;
};

// What one inbound message turns into: possibly a note, possibly a request
// (in that push order), or nothing at all for messages the spec says to
// ignore silently.
struct Translation {
    std::optional<GatewayNote>   note = std::nullopt;
    std::optional<EngineRequest> request = std::nullopt; // timestamps left 0 for the caller to stamp
};

class OrderEntry {
public:
    explicit OrderEntry(validation::PriceBand band) : band_(band) {}

    OrderEntry(const OrderEntry &) = delete;
    OrderEntry &operator=(const OrderEntry &) = delete;

    Translation translate(ClientId client_id, const OuchInboundFrame &msg);

private:
    struct LiveOrder {
        OrderId     order_id;
        OrderRecord record;
    };

    struct ClientState {
        uint32_t last_user_ref_num = 0; // UserRefNums start at 1
    };

    Translation enter_(ClientId client_id, const OuchEnterOrder &msg);
    Translation cancel_(ClientId client_id, const OuchCancelOrder &msg) const;
    Translation replace_(ClientId client_id, const OuchReplaceOrder &msg);

    static uint64_t key_(ClientId client_id, uint32_t user_ref_num) {
        return (static_cast<uint64_t>(client_id) << 32) | user_ref_num;
    }

    validation::PriceBand band_;
    OrderId next_order_id_ = 1;

    absl::flat_hash_map<ClientId, ClientState> clients_;

    // (ClientId, UserRefNum) -> order. Never erased: UserRefNums are
    // day-unique, so a dead entry can't be confused with a new order, and
    // a Cancel/Replace that resolves to a dead order is exactly what the
    // engine's silent-ignore handling exists for. The cost is one entry
    // per order entered today.
    absl::flat_hash_map<uint64_t, LiveOrder> orders_;
};

#endif // ORDER_ENTRY_H
