//
// Created by cniew on 5/24/26.
//
// Singleton order factory.  One global RNG + GBM mid-price shared across all
// clients.  Call OrderFactory::instance() (passing an optional seed on the
// first call) to obtain the process-wide instance.
//
// Builds OUCH 5.0 inbound messages; the caller frames them in SoupBinTCP
// Unsequenced Data packets. Prices are generated in integer cents (the
// exchange's tick) and sent as OUCH's 4-implied-decimal prices.
//
// Probability table (uniform roll over [0, 99]):
//   r > 20  (79/100) -> ENTER (half limit/Day, half market/IOC)
//   r >  8  (12/100) -> CANCEL  (of a live order, in full)
//   r <= 8  ( 9/100) -> REPLACE (of a live order: new price and size, same side)
// CANCEL/REPLACE fall back to ENTER when the client has no live order yet.
// INVALID (an Enter the exchange must reject) is disabled -- see build_message.
//

#ifndef ORDER_FACTORY_H
#define ORDER_FACTORY_H

#include "ouch_proto.hpp"
#include "order_types.hpp"
#include "validation.hpp"
#include "config.hpp"

#include <random>
#include <cmath>
#include <algorithm>
#include <optional>

class OrderFactory {
public:
    enum class FrameKind : uint8_t { ENTER, CANCEL, REPLACE, INVALID };

    // What build_message wants to target with a CANCEL/REPLACE.
    struct Target {
        uint32_t user_ref_num;
        Side     side;
    };

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

    // Builds the next message into out. user_ref_num is what an ENTER or
    // REPLACE will use as its (new) UserRefNum -- the caller advances its
    // counter for those kinds only. target is a live order to CANCEL/REPLACE.
    FrameKind build_message(uint32_t user_ref_num, std::optional<Target> target, OuchInboundFrame& out);

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
    std::uniform_int_distribution<uint32_t> big_qty_dist_ {1'000'000u, 2'000'000u};

    // ---- mid-price state ----
    double mid_price_   = MID_PRICE_INITIAL;
    uint32_t frame_count_ = 0;

    static constexpr OuchStockSymbol kSymbol   = wire::alpha<8>(CLIENT_SYMBOL);
    static constexpr OuchClOrdId     kClOrdId  = wire::alpha<14>("LOADGEN");
    // The spec's market-order price (OUCH 5.0, Data Types).
    static constexpr uint64_t        kMarketPrice = 0x7FFFFFFF;

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
            std::clamp(qty_dist_(rng_), 1.0, double{validation::kOuchMaxQuantity}));
    }

    OuchInboundFrame make_enter(const uint32_t user_ref_num) {
        const auto side   = static_cast<Side>(side_dist_(rng_));
        const bool market = static_cast<OrderType>(type_dist_(rng_)) == OrderType::MARKET;

        OuchInboundFrame frame{};
        frame.enter_order = OuchEnterOrder{};
        OuchEnterOrder& m = frame.enter_order;
        m.user_ref_num  = user_ref_num;
        m.side          = validation::to_ouch_side(side);
        m.quantity      = order_qty();
        m.symbol        = kSymbol;
        m.price         = market ? kMarketPrice : validation::to_ouch_price(limit_price(side));
        m.time_in_force = market ? '3' : '0'; // a market order never rests anyway
        m.display       = 'Y';
        m.capacity      = 'A';
        m.intermarket_sweep_eligibility = 'N';
        m.cross_type    = 'N';
        m.cl_ord_id     = kClOrdId;
        return frame;
    }

    OuchInboundFrame make_cancel(const Target target) {
        OuchInboundFrame frame{};
        frame.cancel_order = OuchCancelOrder{};
        frame.cancel_order.user_ref_num = target.user_ref_num;
        frame.cancel_order.quantity     = 0; // cancel everything still open
        return frame;
    }

    OuchInboundFrame make_replace(const uint32_t user_ref_num, const Target target) {
        OuchInboundFrame frame{};
        frame.replace_order = OuchReplaceOrder{};
        OuchReplaceOrder& m = frame.replace_order;
        m.orig_user_ref_num = target.user_ref_num;
        m.user_ref_num      = user_ref_num;
        m.quantity          = order_qty();
        m.price             = validation::to_ouch_price(limit_price(target.side));
        m.time_in_force     = '0';
        m.display           = 'Y';
        m.intermarket_sweep_eligibility = 'N';
        m.cl_ord_id         = kClOrdId;
        return frame;
    }

    // An Enter the exchange must reject: quantity at or past the spec's 1,000,000 limit.
    OuchInboundFrame make_invalid(const uint32_t user_ref_num) {
        OuchInboundFrame frame = make_enter(user_ref_num);
        frame.enter_order.quantity = big_qty_dist_(rng_);
        return frame;
    }
};

inline OrderFactory::FrameKind OrderFactory::build_message(
        const uint32_t user_ref_num, const std::optional<Target> target, OuchInboundFrame& out) {

    // GBM mid-price step every MID_PRICE_UPDATE_N frames.
    // Bitmask requires MID_PRICE_UPDATE_N to be a power of two.
    if ((++frame_count_ & (MID_PRICE_UPDATE_N - 1)) == 0)
        mid_price_ *= std::exp(log_return_dist_(rng_));

    const int r = roll_dist_(rng_);

    if (r > 20 || !target) {
        out = make_enter(user_ref_num);
        return FrameKind::ENTER;
    }
    if (r > 8) {
        out = make_cancel(*target);
        return FrameKind::CANCEL;
    }
    // r in [0, 8]: REPLACE (INVALID disabled -- re-enable the block below if needed)
    out = make_replace(user_ref_num, *target);
    return FrameKind::REPLACE;

    /*
    out = make_invalid(user_ref_num);
    return FrameKind::INVALID;
    */
}

#endif // ORDER_FACTORY_H
