//
// Orderbook — an autonomous price-time-priority order book.
//
// The Orderbook owns the full book state (the OrderPool, the per-price
// PriceLevelQueues, and the OrderId -> Order* lookup map) and all of the
// mutating logic: entering, reducing, replacing, and matching orders.
//
// It is driven exclusively through process(EngineRequest), with Nasdaq OUCH
// semantics (see communication_proto.hpp for the request/event contract):
//   * an incoming order (NEW, or the new half of a REPLACE) matches against
//     the opposite side first, at each resting order's price; only what's
//     left rests (DAY limit) or is canceled (IOC, MARKET)
//   * CANCEL reduces an order to a new intended size, keeping its time
//     priority; REPLACE is a cancel plus a new order and loses it
//   * requests against a dead/unknown order, superfluous cancels, and
//     anything else the spec says to ignore are dropped without an event
// Every resulting EngineEvent is pushed onto the outbound ring, and every
// change to the visible book is published as ITCH on the market data ring.
//

#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include "order.hpp"
#include "price_level_queue.hpp"
#include "order_pool.hpp"
#include "order_types.hpp"
#include "communication_proto.hpp"
#include "itch_proto.hpp"
#include "ring_buffer.hpp"
#include "tsc_clock.hpp"
#include "config.hpp"

#include <array>
#include <unordered_map>

//=============================================================================
// Book storage types
//=============================================================================
//
// The map does NOT own the Order objects — it stores non-owning pointers into
// the OrderPool arena. Ownership is centralised in the pool (see order_pool.hpp).
// The bid/ask arrays are indexed by (price - MIN_PRICE): one PriceLevelQueue
// per tick.

using OrderMap  = std::unordered_map<OrderId, Order*>;
using BidLevels = std::array<PriceLevelQueue, MAX_PRICE - MIN_PRICE + 1>;
using AskLevels = std::array<PriceLevelQueue, MAX_PRICE - MIN_PRICE + 1>;

//=============================================================================
// Orderbook
//=============================================================================

class Orderbook {
public:
    Orderbook(OutboundRing& out_ring, MarketDataRing& market_data_ring);

    Orderbook(const Orderbook&)            = delete;
    Orderbook& operator=(const Orderbook&) = delete;
    Orderbook(Orderbook&&)                 = delete;
    Orderbook& operator=(Orderbook&&)      = delete;

    ~Orderbook() = default;

    // Route point: apply a single request to the book. engine_pop_tsc is when
    // the engine took req off its ring; it stamps every event req produces
    // and dates its ITCH. Returns false if req was ignored (no events, no
    // book change).
    bool process(const EngineRequest& req, Timestamp engine_pop_tsc);

    // Software prefetch: warm the structures the next request will touch (the
    // price-level slot for NEW, the orders_ bucket for CANCEL/REPLACE) while the
    // current request is still being processed.
    void prefetch(const EngineRequest& next) const;

#if TESTING
    size_t order_count()     const { return orders_.size(); }
    size_t bid_level_count() const { return non_empty_bid_levels_; }
    size_t ask_level_count() const { return non_empty_ask_levels_; }
    size_t bids_at(Price p)  const { return bids_[p - MIN_PRICE].size(); }
    size_t asks_at(Price p)  const { return asks_[p - MIN_PRICE].size(); }
    bool   has_order(OrderId id) const { return orders_.contains(id); }
    Quantity open_quantity(OrderId id) const { return orders_.at(id)->open_quantity_; }
    Quantity executed_quantity(OrderId id) const { return orders_.at(id)->executed_quantity_; }
    uint64_t market_data_drops() const { return market_data_drops_; }
#endif

private:
    // The request currently being processed, plus the per-request stamps
    // every event and ITCH message it produces shares.
    struct Context {
        const EngineRequest& req;
        Timestamp engine_pop_tsc;
        uint64_t  itch_timestamp; // ns since midnight
        mutable bool answered = false; // set by the request's first emit()
    };

    //===== data containers + facilitating members ======
    OrderPool pool_;   // owns every Order; must outlive orders_/bids_/asks_ pointers
    BidLevels bids_;
    AskLevels asks_;
    OrderMap  orders_; // OrderId -> Order* into pool_ (non-owning)

    Price  best_bid_price_{MIN_PRICE};
    Price  best_ask_price_{MAX_PRICE};
    size_t non_empty_bid_levels_{0};
    size_t non_empty_ask_levels_{0};

    uint64_t next_match_number_{1};

    //===== output ======
    OutboundRing&   out_ring_;
    MarketDataRing& market_data_ring_;
    const TscClock& clock_;
    uint64_t        market_data_drops_{0};

    //===== request handlers ======
    bool enter(const Context& ctx);
    bool cancel(const Context& ctx);
    bool replace(const Context& ctx);

    //===== matching ======
    // Matches an incoming order against the opposite side, best price first,
    // until it's filled or the book no longer crosses. Returns shares filled.
    Quantity match(const Context& ctx, OrderId id, ClientId client_id, Side side,
                   OrderType type, Price price, Quantity quantity);
    bool crosses(Side side, OrderType type, Price price) const;

    //===== book maintenance ======
    // Books a new resting order (allocated from the pool) and publishes its ITCH
    // Add, unless publish_add is false (a replace publishes ITCH Replace instead).
    void rest(const Context& ctx, const Order& order, bool publish_add = true);
    void unlink(Order* order);  // off its price level; best prices updated
    void release(Order* order); // out of orders_ and back to the pool

    void update_best_bid();
    void update_best_ask();

    //===== output ======
    // out_ring_ is a large SPSC ring drained continuously by the outbound
    // thread, so a full ring only means a transient backpressure spike, not a
    // stuck consumer — spin rather than silently drop an event.
    void emit(const Context& ctx, EngineEvent event);
    // Nothing consumes market data yet, so a full ring drops (and counts)
    // rather than stalling matching.
    void publish(const ITCH& msg);
    ItchHeader itch_header(const Context& ctx, ItchMessageType type) const;
};

#endif // ORDERBOOK_H
