//
// OUCH-level validation and OUCH <-> engine field conversion. Pure functions,
// no exchange state, so they can be unit-tested directly; the caller passes
// in the engine's price band rather than this header reaching into the
// exchange's config.hpp.
//

#ifndef VALIDATION_H
#define VALIDATION_H

#include "order_types.hpp"
#include "ouch_proto.hpp"

#include <cstdint>
#include <optional>

namespace validation {

// Inclusive range of prices the book can hold, in engine cents.
struct PriceBand {
    Price min;
    Price max;
};

inline constexpr uint32_t kOuchMaxQuantity = 999'999; // spec: > 0 and < 1,000,000

// OUCH prices carry 4 implied decimals, engine prices are cents.
inline constexpr uint64_t kOuchTicksPerCent = 100;

struct EnginePrice {
    OrderType type;
    Price     price; // 0 for MARKET
};

// OUCH price -> engine price. A price at or above kOuchMarketPriceFloor is a
// market order. nullopt for a sub-penny price or one outside band (the
// only prices between the max limit price and the market floor are
// sub-penny).
inline std::optional<EnginePrice> to_engine_price(const uint64_t ouch_price, const PriceBand band) noexcept {
    if (ouch_price >= kOuchMarketPriceFloor) return EnginePrice{OrderType::MARKET, 0};
    if (ouch_price > kOuchMaxLimitPrice || ouch_price % kOuchTicksPerCent != 0) return std::nullopt;

    const uint64_t cents = ouch_price / kOuchTicksPerCent;
    if (cents < band.min || cents > band.max) return std::nullopt;
    return EnginePrice{OrderType::LIMIT, static_cast<Price>(cents)};
}

inline uint64_t to_ouch_price(const Price cents) noexcept { return uint64_t{cents} * kOuchTicksPerCent; }

// ITCH prices are 4 bytes with 4 implied decimals; any band that fits the
// engine's dense price ladder is far below the uint32 limit of $429,496.
inline uint32_t to_itch_price(const Price cents) noexcept {
    return static_cast<uint32_t>(cents * kOuchTicksPerCent);
}

// Short sells ('T'/'E') aren't supported.
inline std::optional<Side> to_engine_side(const char side) noexcept {
    switch (side) {
        case 'B': return Side::BID;
        case 'S': return Side::ASK;
        default:  return std::nullopt;
    }
}

inline char to_ouch_side(const Side side) noexcept { return side == Side::BID ? 'B' : 'S'; }

// Day and IOC only; GTX/GTT/after-hours aren't supported.
inline std::optional<TimeInForce> to_engine_tif(const char tif) noexcept {
    switch (tif) {
        case '0': return TimeInForce::DAY;
        case '3': return TimeInForce::IOC;
        default:  return std::nullopt;
    }
}

namespace detail {

inline bool valid_quantity(const uint32_t quantity) noexcept {
    return quantity > 0 && quantity <= kOuchMaxQuantity;
}

// Optional fields this book doesn't implement are accepted only at their
// spec default (whether or not the tag was sent). Shared by Enter and
// Replace, which spell these fields identically.
template <typename M>
uint16_t unsupported_option_reason(const M &m) noexcept {
    if (m.min_qty != 0) return ouch_reject::kInvalidMinQuantity;
    if (m.max_floor != 0) return ouch_reject::kInvalidMaxFloor;
    if (m.price_type != 'L' || m.peg_offset != 0 || m.discretion_price != 0 ||
        m.discretion_price_type != 'L' || m.discretion_peg_offset != 0) {
        return ouch_reject::kInvalidPegType;
    }
    if (m.post_only != 'N' || m.random_reserves != 0 || m.expire_time != 0 || m.trade_now != 'N' ||
        m.handle_inst != ' ' || m.aiq_strategy != '*' || m.aiq_group_id != OuchAiqGroupId{'*', '*'} ||
        m.appendage.has(OuchTag::USER_REF_IDX)) { // order-flow channels: UserRefNum is per port here
        return ouch_reject::kOther;
    }
    return 0;
}

} // namespace detail

// What an accepted Enter Order becomes on the engine side.
struct EnterCheck {
    uint16_t    reject_reason = 0; // 0 = accepted; otherwise an ouch_reject code
    Side        side{};
    OrderType   order_type{};
    Price       price = 0;
    TimeInForce time_in_force{};
};

// Firm, CustomerType and GroupID are accepted with any value; they don't
// affect matching.
inline EnterCheck check_enter(const OuchEnterOrder &m, const PriceBand band) noexcept {
    EnterCheck check;
    const auto reject = [&](const uint16_t reason) {
        check.reject_reason = reason;
        return check;
    };

    const std::optional<Side> side = to_engine_side(m.side);
    if (!side) return reject(ouch_reject::kInvalidSide);
    if (!detail::valid_quantity(m.quantity)) return reject(ouch_reject::kInvalidQuantity);

    const std::optional<EnginePrice> price = to_engine_price(m.price, band);
    if (!price) return reject(ouch_reject::kInvalidPrice);

    const std::optional<TimeInForce> tif = to_engine_tif(m.time_in_force);
    if (!tif) return reject(ouch_reject::kOther);
    if (m.display != 'Y') return reject(ouch_reject::kInvalidDisplay);
    if (m.capacity != 'A' && m.capacity != 'P' && m.capacity != 'R' && m.capacity != 'O') {
        return reject(ouch_reject::kOther);
    }
    if (m.intermarket_sweep_eligibility != 'N') return reject(ouch_reject::kIsoNotAllowed);
    if (m.cross_type != 'N') return reject(ouch_reject::kInvalidCrossOrder);
    if (const uint16_t reason = detail::unsupported_option_reason(m); reason != 0) return reject(reason);

    check.side = *side;
    check.order_type = price->type;
    check.price = price->price;
    check.time_in_force = *tif;
    return check;
}

// What a valid Replace Order becomes on the engine side. Invalid replaces
// aren't rejected: per spec they cancel the existing order instead.
struct ReplaceCheck {
    bool        valid = false;
    Price       price = 0;
    TimeInForce time_in_force{};
};

inline ReplaceCheck check_replace(const OuchReplaceOrder &m, const char orig_side, const PriceBand band) noexcept {
    ReplaceCheck check;
    if (!detail::valid_quantity(m.quantity)) return check;

    // A replace can't turn a resting order into a market order.
    const std::optional<EnginePrice> price = to_engine_price(m.price, band);
    if (!price || price->type != OrderType::LIMIT) return check;

    const std::optional<TimeInForce> tif = to_engine_tif(m.time_in_force);
    if (!tif) return check;
    if (m.display != 'Y' || m.intermarket_sweep_eligibility != 'N') return check;
    if (m.appendage.has(OuchTag::SIDE) && m.side != orig_side) return check;
    if (detail::unsupported_option_reason(m) != 0) return check;

    check.valid = true;
    check.price = price->price;
    check.time_in_force = *tif;
    return check;
}

} // namespace validation

#endif // VALIDATION_H
