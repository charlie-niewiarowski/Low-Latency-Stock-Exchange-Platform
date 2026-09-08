//
// Validation logic extracted from server.cpp so it can be unit-tested directly.
//

#ifndef VALIDATION_H
#define VALIDATION_H

#include "communication_types.hpp"
#include "config.hpp"

inline bool validate_new(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 0) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    // Price is used directly as a (price - MIN_PRICE) index into the book's
    // fixed-size price ladder, so an out-of-range LIMIT price must be
    // rejected here rather than reaching Orderbook and indexing out of bounds.
    if (msg.order_type == OrderType::LIMIT &&
        (msg.price < MIN_PRICE || msg.price > MAX_PRICE)) return false;

    if (msg.quantity > 1'000'000u) return false;

    return true;
}

inline bool validate_cancel(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 1) return false;
    return true;
}

inline bool validate_modify(const InboundMessage &msg) {
    auto raw_type = static_cast<uint8_t>(msg.message_type);
    if (raw_type != 2) return false;

    auto raw_side = static_cast<uint8_t>(msg.side);
    if (raw_side != 0 && raw_side != 1) return false;

    auto raw_order_type = static_cast<uint8_t>(msg.order_type);
    if (raw_order_type != 0 && raw_order_type != 1) return false;

    if (msg.order_type == OrderType::LIMIT &&
        (msg.price < MIN_PRICE || msg.price > MAX_PRICE)) return false;

    if (msg.quantity > 1'000'000u) return false;

    return true;
}

inline bool validate_message(const InboundMessage &msg) {
    switch (msg.message_type) {
        case MessageType::NEW:    return validate_new(msg);
        case MessageType::CANCEL: return validate_cancel(msg);
        case MessageType::MODIFY: return validate_modify(msg);
        default:                  return false;
    }
}

#endif // VALIDATION_H