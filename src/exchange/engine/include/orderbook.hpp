//
// Orderbook — an autonomous price-time-priority order book.
//
// The Orderbook owns the full book state (the OrderPool, the per-price
// PriceLevelQueues, and the OrderId -> Order* lookup map) and all of the
// mutating logic: adding, cancelling, modifying, and matching orders.
//
// It is driven exclusively through process(OrderRequest): the Engine translates
// each inbound wire message into an OrderRequest and hands it over. Matching
// results (MATCH fills) are pushed directly onto the outbound ring the Orderbook
// was constructed with; process() returns a ProcessResult telling the caller how
// to acknowledge the request.
//

#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include "order.hpp"
#include "price_level_queue.hpp"
#include "order_pool.hpp"
#include "order_request.hpp"
#include "order_types.hpp"
#include "communication_types.hpp"
#include "ring_buffer.hpp"
#include "trade.hpp"
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
// ProcessResult — how the caller should acknowledge a processed request
//=============================================================================
//
// Every request gets exactly one direct ack/error, regardless of whether it
// also produced MATCH fills (those are pushed onto the outbound ring
// separately, to the affected counterparties). The client's pipeline-depth
// accounting frees a connection's in-flight slot only on that direct
// response — never on a MATCH — so a request that filled but was never
// acked would permanently strand a pipeline slot.

struct ProcessResult {
    Status status;
};

//=============================================================================
// Orderbook
//=============================================================================

class Orderbook {
public:
    explicit Orderbook(OutboundRing& out_ring);

    Orderbook(const Orderbook&)            = delete;
    Orderbook& operator=(const Orderbook&) = delete;
    Orderbook(Orderbook&&)                 = delete;
    Orderbook& operator=(Orderbook&&)      = delete;

    ~Orderbook() = default;

    // Route point: apply a single request to the book.
    ProcessResult process(const OrderRequest& req);

    // Software prefetch: warm the structures the next request will touch (the
    // price-level slot for NEW, the orders_ bucket for CANCEL/MODIFY) while the
    // current request is still being processed.
    void prefetch(const InboundMessage& next) const;

#if LOGGING || TESTING
    // Drain a trade produced by matching (consumed by the Engine's expose thread,
    // and by engine-tests, which builds with TESTING=1 but LOGGING=0).
    [[nodiscard]] std::optional<Trade> pop_trade() { return trades_ring_.pop(); }
#endif

#if TESTING
    size_t order_count()     const { return orders_.size(); }
    size_t bid_level_count() const { return non_empty_bid_levels_; }
    size_t ask_level_count() const { return non_empty_ask_levels_; }
    size_t bids_at(Price p)  const { return bids_[p - MIN_PRICE].size(); }
    size_t asks_at(Price p)  const { return asks_[p - MIN_PRICE].size(); }
    bool   has_order(OrderId id) const { return orders_.contains(id); }
#endif

private:
    //===== data containers + facilitating members ======
    OrderPool pool_;   // owns every Order; must outlive orders_/bids_/asks_ pointers
    BidLevels bids_;
    AskLevels asks_;
    OrderMap  orders_; // OrderId -> Order* into pool_ (non-owning)

    Price  best_bid_price_{MIN_PRICE};
    Price  best_ask_price_{MAX_PRICE};
    size_t non_empty_bid_levels_{0};
    size_t non_empty_ask_levels_{0};

    //===== output ======
    OutboundRing& out_ring_;   // MATCH fills are pushed here
#if LOGGING || TESTING
    RingBuffer<Trade> trades_ring_{TRADE_RING_COUNT};
#endif

    //===== state modifications =====
    bool addOrder(const OrderRequest& order_request);
    void cancelOrder(OrderId id);
    bool modifyOrder(const OrderRequest& order_request);

    void update_best_bid();
    void update_best_ask();

    //===== matching ======
    bool matchOrders();
    bool matchMarket(const OrderRequest& order_request);
    bool canMatch(Side side, Price price) const;

    // out_ring_ is a large SPSC ring drained continuously by the outbound
    // thread, so a full ring only means a transient backpressure spike, not a
    // stuck consumer — spin rather than silently drop a fill/trade record.
    void pushOut(const OutboundMessage& msg);
#if LOGGING || TESTING
    void pushTrade(const Trade& trade);
#endif
};

#endif // ORDERBOOK_H
