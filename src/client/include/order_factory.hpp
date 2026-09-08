//
// Created by cniew on 5/24/26.
//
// Singleton order factory.  One global RNG + GBM mid-price shared across all
// clients.  Call OrderFactory::instance() (passing an optional seed on the
// first call) to obtain the process-wide instance.
//
// Wire frame layout (INBOUND_BSIZE = 72 bytes):
//   "EXCHANGE\n"   (9 bytes)
//   InboundMessage (56 bytes)
//   '\n'           (1 byte)
//   padding        (6 bytes)
//
// Probability table (uniform roll over [0, 99]):
//   r > 25  (74/100) -> NEW
//   r > 10  (14/100) -> CANCEL
//   r >  3  ( 7/100) -> MODIFY
//   r <= 3  ( 4/100) -> INVALID / GARBAGE (coin flip decides which)
//

#ifndef ORDER_FACTORY_H
#define ORDER_FACTORY_H

#include "communication_types.hpp"
#include "protocol.hpp"
#include "config.hpp"

#include <random>
#include <cmath>
#include <algorithm>
#include <cstring>

class OrderFactory {
public:
    enum class FrameKind : uint8_t { NEW, CANCEL, MODIFY, GARBAGE, INVALID };

    // Pass a non-zero seed on the very first call to fix the RNG sequence.
    // Subsequent calls ignore the argument and return the existing instance.
    static OrderFactory& instance(uint64_t seed = 0) {
        static OrderFactory inst(seed != 0 ? seed
                                           : static_cast<uint64_t>(std::random_device{}()));
        return inst;
    }

    OrderFactory(const OrderFactory&)            = delete;
    OrderFactory& operator=(const OrderFactory&) = delete;
    OrderFactory(OrderFactory&&)                 = delete;
    OrderFactory& operator=(OrderFactory&&)      = delete;

    FrameKind build_frame(ClientId cid, OrderId active_order,
                          char* dst, InboundMessage& msg_out);

private:
    explicit OrderFactory(uint64_t seed) : rng_(seed) {}

    // ---- RNG ----
    std::mt19937_64 rng_;

    // ---- distributions (in-class initializers use config macros) ----
    std::normal_distribution<double>    log_return_dist_{0.0, MID_PRICE_VOL};
    std::lognormal_distribution<double> qty_dist_{std::log(MEAN_QTY), QTY_VOL};
    std::bernoulli_distribution         side_dist_{0.5};
    std::bernoulli_distribution         type_dist_{0.5};
    std::uniform_int_distribution<int>      roll_dist_    {0, 99};
    std::uniform_int_distribution<uint8_t>  byte_dist_    {0, 255};
    std::uniform_int_distribution<uint32_t> big_qty_dist_ {1'000'001u, 2'000'000u};
    std::bernoulli_distribution             coin_         {0.5};

    // ---- mid-price state ----
    double mid_price_   = MID_PRICE_INITIAL;
    uint32_t frame_count_ = 0;

    // ---- helpers ----
    // Reuses log_return_dist_ for the spread offset (avoids a second distribution).
    Price limit_price(Side side) {
        const double spread = std::abs(log_return_dist_(rng_)) * mid_price_;
        const double raw    = (side == Side::BID) ? mid_price_ - spread
                                                  : mid_price_ + spread;
        return static_cast<Price>(std::clamp(raw, double{MIN_PRICE}, double{MAX_PRICE}));
    }

    Quantity order_qty() {
        return static_cast<Quantity>(
            std::clamp(qty_dist_(rng_), 1.0, 1'000'000.0));
    }

    InboundMessage make_new(ClientId cid) {
        const auto side       = static_cast<Side>     (side_dist_(rng_));
        const auto order_type = static_cast<OrderType>(type_dist_(rng_));
        return InboundMessage{
            .order_id     = 0,
            .client_id    = cid,
            .price        = (order_type == OrderType::LIMIT) ? limit_price(side) : Price{0},
            .quantity     = order_qty(),
            .message_type = MessageType::NEW,
            .side         = side,
            .order_type   = order_type,
        };
    }

    InboundMessage make_cancel(ClientId cid, OrderId target_oid) {
        return InboundMessage{
            .order_id     = target_oid,
            .client_id    = cid,
            .price        = 0,
            .quantity     = 0,
            .message_type = MessageType::CANCEL,
            .side         = Side::BID,
            .order_type   = OrderType::LIMIT,
        };
    }

    InboundMessage make_modify(ClientId cid, OrderId target_oid) {
        const auto side = static_cast<Side>(side_dist_(rng_));
        return InboundMessage{
            .order_id     = target_oid,
            .client_id    = cid,
            .price        = limit_price(side),
            .quantity     = order_qty(),
            .message_type = MessageType::MODIFY,
            .side         = side,
            .order_type   = OrderType::LIMIT,
        };
    }

    InboundMessage make_invalid_body(ClientId cid) {
        if (coin_(rng_)) {
            return InboundMessage{
                .order_id     = 0,
                .client_id    = cid,
                .price        = 0,       // LIMIT + price==0 → invalid
                .quantity     = 100,
                .message_type = MessageType::NEW,
                .side         = Side::BID,
                .order_type   = OrderType::LIMIT,
            };
        }
        return InboundMessage{
            .order_id     = 0,
            .client_id    = cid,
            .price        = 100,
            .quantity     = big_qty_dist_(rng_),  // > 1 000 000 → invalid
            .message_type = MessageType::NEW,
            .side         = Side::BID,
            .order_type   = OrderType::LIMIT,
        };
    }

    static void frame_message(const InboundMessage& msg, char* dst) {
        constexpr size_t hlen = sizeof("EXCHANGE\n") - 1;  // 9
        std::memcpy(dst,         "EXCHANGE\n", hlen);
        std::memcpy(dst + hlen,  &msg,         sizeof(msg));
        dst[hlen + sizeof(msg)] = '\n';
    }
};

OrderFactory::FrameKind OrderFactory::build_frame(
        const ClientId cid, const OrderId active_order,
        char* dst, InboundMessage& msg_out) {

    // GBM mid-price step every MID_PRICE_UPDATE_N frames.
    // Bitmask requires MID_PRICE_UPDATE_N to be a power of two.
    if ((++frame_count_ & (MID_PRICE_UPDATE_N - 1)) == 0)
        mid_price_ *= std::exp(log_return_dist_(rng_));

    const int r = roll_dist_(rng_);
    msg_out = {};

    if (r > 20) {
        msg_out = make_new(cid);
        frame_message(msg_out, dst);
        return FrameKind::NEW;
    }
    if (r > 8) {
        msg_out = make_cancel(cid, active_order);
        frame_message(msg_out, dst);
        return FrameKind::CANCEL;
    }
    // r in [0, 8]: MODIFY (GARBAGE/INVALID disabled — re-enable the block below if needed)
    msg_out = make_modify(cid, active_order);
    frame_message(msg_out, dst);
    return FrameKind::MODIFY;

    /*
    if (coin_(rng_)) {
        msg_out = make_invalid_body(cid);
        frame_message(msg_out, dst);
        return FrameKind::INVALID;
    }
    for (size_t i = 0; i < INBOUND_BSIZE; ++i)
        dst[i] = static_cast<char>(byte_dist_(rng_));
    return FrameKind::GARBAGE;
    */
}

#endif // ORDER_FACTORY_H