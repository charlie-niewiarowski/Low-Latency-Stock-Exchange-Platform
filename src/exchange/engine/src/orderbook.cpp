//
// Orderbook — autonomous price-time-priority book (see orderbook.hpp).
//

#include "../include/orderbook.hpp"
#include "perf.hpp"
#include "validation.hpp"

#include <immintrin.h>

#include <algorithm>

namespace {

constexpr ItchStockSymbol kBookSymbol = wire::alpha<8>(BOOK_SYMBOL);

// Shares left after a request that sets an order chain's total liability
// (CANCEL's intended size, REPLACE's quantity -- both inclusive of shares
// already executed on the chain).
Quantity open_after(const Quantity total_liability, const Quantity executed) {
    return total_liability > executed ? total_liability - executed : 0;
}

} // namespace

//=============================================================================
// construction
//=============================================================================

Orderbook::Orderbook(OutboundRing& out_ring, MarketDataRing& market_data_ring)
    : pool_(PREALLOCATION_COUNT), out_ring_(out_ring), market_data_ring_(market_data_ring),
      clock_(TscClock::instance()) {
    orders_.reserve(PREALLOCATION_COUNT);
}

//=============================================================================
// process() — the single entry point
//=============================================================================

bool Orderbook::process(const EngineRequest& req, const Timestamp engine_pop_tsc) {
    const Context ctx{req, engine_pop_tsc, clock_.ns_since_midnight(engine_pop_tsc)};

    switch (req.type) {
        case EngineRequestType::NEW:     return enter(ctx);
        case EngineRequestType::CANCEL:  return cancel(ctx);
        case EngineRequestType::REPLACE: return replace(ctx);
    }
    return false; // defensive: only reachable via an out-of-range enum value
}

//=============================================================================
// prefetch() — warm the structures the next request will touch
//=============================================================================

void Orderbook::prefetch(const EngineRequest& next) const {
    switch (next.type) {
        case EngineRequestType::NEW: {
            // The price ladder is a ~2.4 MB sparse array indexed by tick; warm the
            // slot the order would rest in. Guard the index (prefetch never faults,
            // but keep the pointer in-bounds) -- MARKET orders carry price 0.
            if (next.price < MIN_PRICE || next.price > MAX_PRICE) return;
            const size_t idx = next.price - MIN_PRICE;
            prefetch_read(next.side == Side::BID ? &bids_[idx] : &asks_[idx]);
            break;
        }
        case EngineRequestType::CANCEL:
        case EngineRequestType::REPLACE: {
            // std::unordered_map exposes only the bucket index, so this is an
            // early-touch of the bucket's first node rather than a pure async
            // prefetch (see the plan's map-node caveat).
            if (orders_.bucket_count() == 0) return;
            const size_t b = orders_.bucket(next.order_id);
            const auto lit = orders_.begin(b);
            if (lit != orders_.end(b)) prefetch_read(&*lit);
            break;
        }
    }
}

//=============================================================================
// request handlers
//=============================================================================

// NEW: ACCEPTED, then match as the aggressor, then rest a DAY limit
// remainder or cancel any other remainder (IOC / MARKET).
bool Orderbook::enter(const Context& ctx) {
    const EngineRequest& req = ctx.req;
    // OrderIds are gateway-assigned and unique; a repeat is a duplicate.
    if (orders_.contains(req.order_id)) return false;

    const bool may_rest = req.order_type == OrderType::LIMIT && req.time_in_force == TimeInForce::DAY;
    if (may_rest && pool_.available() == 0) {
        emit(ctx, {.order_id = req.order_id, .client_id = req.client_id,
                   .type = EngineEventType::REJECTED, .reason = engine_reason::kBookFull});
        return true;
    }

    emit(ctx, {.order_id = req.order_id, .client_id = req.client_id, .price = req.price,
               .quantity = req.quantity, .type = EngineEventType::ACCEPTED});

    const Quantity filled = match(ctx, req.order_id, req.client_id, req.side, req.order_type,
                                  req.price, req.quantity);
    const Quantity remaining = req.quantity - filled;
    if (remaining == 0) return true;

    if (may_rest) {
        rest(ctx, Order{req.order_id, req.client_id, req.side, req.price, remaining, filled});
    } else {
        emit(ctx, {.order_id = req.order_id, .client_id = req.client_id, .price = req.price,
                   .quantity = remaining, .type = EngineEventType::CANCELED,
                   .reason = engine_reason::kImmediateOrCancel});
    }
    return true;
}

// CANCEL: reduce to the intended size, in place (time priority kept), or
// remove entirely at 0. A cancel that wouldn't reduce anything is
// superfluous and ignored.
bool Orderbook::cancel(const Context& ctx) {
    const EngineRequest& req = ctx.req;
    const auto it = orders_.find(req.order_id);
    if (it == orders_.end() || it->second->client_id_ != req.client_id) return false;

    Order* order = it->second;
    const Quantity target_open = open_after(req.quantity, order->executed_quantity_);
    if (target_open >= order->open_quantity_) return false;

    const Quantity decrement = order->open_quantity_ - target_open;
    emit(ctx, {.order_id = order->id_, .client_id = order->client_id_, .price = order->price_,
               .quantity = decrement, .type = EngineEventType::CANCELED,
               .reason = engine_reason::kUserRequested});

    ITCH itch;
    if (target_open == 0) {
        itch.order_delete = ItchOrderDelete{.header = itch_header(ctx, ItchMessageType::ORDER_DELETE),
                                            .order_reference_number = order->id_};
        unlink(order);
        release(order);
    } else {
        order->open_quantity_ = target_open;
        itch.order_cancel = ItchOrderCancel{.header = itch_header(ctx, ItchMessageType::ORDER_CANCEL),
                                            .order_reference_number = order->id_,
                                            .canceled_shares = decrement};
    }
    publish(itch);
    return true;
}

// REPLACE: cancel the original and enter the replacement under
// new_order_id at the back of its level, carrying the chain's executed
// shares. The replacement trades like any incoming order; if it would rest
// without trading, ITCH sees one Order Replace instead of Delete + Add.
bool Orderbook::replace(const Context& ctx) {
    const EngineRequest& req = ctx.req;
    const auto it = orders_.find(req.order_id);
    if (it == orders_.end() || it->second->client_id_ != req.client_id) return false;
    if (orders_.contains(req.new_order_id)) return false;

    Order* orig = it->second;
    const OrderId  orig_id  = orig->id_;
    const Side     side     = orig->side_;
    const Quantity executed = orig->executed_quantity_;
    const Quantity open     = open_after(req.quantity, executed);

    ITCH itch;
    itch.order_delete = ItchOrderDelete{.header = itch_header(ctx, ItchMessageType::ORDER_DELETE),
                                        .order_reference_number = orig_id};

    // The chain has already executed everything the replace would allow:
    // nothing is left to replace into, so this amounts to a full cancel.
    if (open == 0) {
        emit(ctx, {.order_id = orig_id, .client_id = orig->client_id_, .price = orig->price_,
                   .quantity = orig->open_quantity_, .type = EngineEventType::CANCELED,
                   .reason = engine_reason::kUserRequested});
        unlink(orig);
        release(orig);
        publish(itch);
        return true;
    }

    unlink(orig);
    release(orig); // frees the pool slot the replacement may need

    emit(ctx, {.order_id = req.new_order_id, .orig_order_id = orig_id, .client_id = req.client_id,
               .price = req.price, .quantity = open, .type = EngineEventType::REPLACED});

    const bool day = req.time_in_force == TimeInForce::DAY;
    if (day && !crosses(side, OrderType::LIMIT, req.price)) {
        rest(ctx, Order{req.new_order_id, req.client_id, side, req.price, open, executed},
             /*publish_add=*/false);
        itch.order_replace = ItchOrderReplace{.header = itch_header(ctx, ItchMessageType::ORDER_REPLACE),
                                              .original_order_reference_number = orig_id,
                                              .new_order_reference_number = req.new_order_id,
                                              .shares = open,
                                              .price = validation::to_itch_price(req.price)};
        publish(itch);
        return true;
    }

    publish(itch); // Delete of the original; the replacement trades first
    const Quantity filled = match(ctx, req.new_order_id, req.client_id, side, OrderType::LIMIT,
                                  req.price, open);
    const Quantity remaining = open - filled;
    if (remaining == 0) return true;

    if (day) {
        rest(ctx, Order{req.new_order_id, req.client_id, side, req.price, remaining, executed + filled});
    } else {
        emit(ctx, {.order_id = req.new_order_id, .client_id = req.client_id, .price = req.price,
                   .quantity = remaining, .type = EngineEventType::CANCELED,
                   .reason = engine_reason::kImmediateOrCancel});
    }
    return true;
}

//=============================================================================
// matching
//=============================================================================

Quantity Orderbook::match(const Context& ctx, const OrderId id, const ClientId client_id, const Side side,
                          const OrderType type, const Price price, const Quantity quantity) {
    Quantity filled = 0;

    while (filled < quantity && crosses(side, type, price)) {
        // Every fill executes at the resting order's price.
        const Price level_price = side == Side::BID ? best_ask_price_ : best_bid_price_;
        PriceLevelQueue& level = side == Side::BID ? asks_[level_price - MIN_PRICE]
                                                   : bids_[level_price - MIN_PRICE];
        Order* resting = level.front();

        const Quantity fill = std::min(quantity - filled, resting->open_quantity_);
        const uint64_t match_number = next_match_number_++;
        filled += fill;
        resting->open_quantity_ -= fill;
        resting->executed_quantity_ += fill;

        emit(ctx, {.order_id = id, .match_number = match_number, .client_id = client_id,
                   .price = level_price, .quantity = fill, .type = EngineEventType::EXECUTED,
                   .reason = engine_reason::kRemovedLiquidity});
        emit(ctx, {.order_id = resting->id_, .match_number = match_number,
                   .client_id = resting->client_id_, .price = level_price, .quantity = fill,
                   .type = EngineEventType::EXECUTED, .reason = engine_reason::kAddedLiquidity});

        // ITCH only reports the resting side: the aggressor was never on the book.
        ITCH itch;
        itch.order_executed = ItchOrderExecuted{.header = itch_header(ctx, ItchMessageType::ORDER_EXECUTED),
                                                .order_reference_number = resting->id_,
                                                .executed_shares = fill,
                                                .match_number = match_number};
        publish(itch);

        if (resting->is_filled()) {
            unlink(resting);
            release(resting);
        }
    }

    return filled;
}

bool Orderbook::crosses(const Side side, const OrderType type, const Price price) const {
    if (side == Side::BID) {
        if (non_empty_ask_levels_ == 0) return false;
        return type == OrderType::MARKET || price >= best_ask_price_;
    }

    if (non_empty_bid_levels_ == 0) return false;
    return type == OrderType::MARKET || price <= best_bid_price_;
}

//=============================================================================
// book maintenance
//=============================================================================

void Orderbook::rest(const Context& ctx, const Order& order, const bool publish_add) {
    // Callers guarantee a free slot: enter() checks up front, replace() frees
    // the original first.
    Order* resting = pool_.allocate(order);
    orders_.emplace(resting->id_, resting);

    const Price price = resting->price_;
    if (resting->side_ == Side::BID) {
        if (bids_[price - MIN_PRICE].empty()) ++non_empty_bid_levels_;
        bids_[price - MIN_PRICE].push(resting);
        if (price > best_bid_price_) best_bid_price_ = price;
    } else {
        if (asks_[price - MIN_PRICE].empty()) ++non_empty_ask_levels_;
        asks_[price - MIN_PRICE].push(resting);
        if (price < best_ask_price_) best_ask_price_ = price;
    }

    if (publish_add) {
        ITCH itch;
        itch.add_order = ItchAddOrder{.header = itch_header(ctx, ItchMessageType::ADD_ORDER),
                                      .order_reference_number = resting->id_,
                                      .buy_sell_indicator = validation::to_ouch_side(resting->side_),
                                      .shares = resting->open_quantity_,
                                      .stock = kBookSymbol,
                                      .price = validation::to_itch_price(price)};
        publish(itch);
    }
}

void Orderbook::unlink(Order* order) {
    const Price price = order->price_;

    if (order->side_ == Side::BID) {
        bids_[price - MIN_PRICE].remove(order);
        if (bids_[price - MIN_PRICE].empty()) {
            --non_empty_bid_levels_;
            if (non_empty_bid_levels_ == 0)    best_bid_price_ = MIN_PRICE;
            else if (price == best_bid_price_) update_best_bid();
        }
    } else {
        asks_[price - MIN_PRICE].remove(order);
        if (asks_[price - MIN_PRICE].empty()) {
            --non_empty_ask_levels_;
            if (non_empty_ask_levels_ == 0)    best_ask_price_ = MAX_PRICE;
            else if (price == best_ask_price_) update_best_ask();
        }
    }
}

void Orderbook::release(Order* order) {
    orders_.erase(order->id_);
    pool_.deallocate(order);   // return the slot to the pool for reuse
}

void Orderbook::update_best_bid() {
    while (best_bid_price_ > MIN_PRICE && bids_[best_bid_price_ - MIN_PRICE].empty()) {
        --best_bid_price_;
    }
}

void Orderbook::update_best_ask() {
    while (best_ask_price_ < MAX_PRICE && asks_[best_ask_price_ - MIN_PRICE].empty()) {
        ++best_ask_price_;
    }
}

//=============================================================================
// output
//=============================================================================

void Orderbook::emit(const Context& ctx, EngineEvent event) {
    event.read_begin_tsc  = ctx.req.read_begin_tsc;
    event.recv_tsc        = ctx.req.recv_tsc;
    event.server_push_tsc = ctx.req.server_push_tsc;
    event.engine_pop_tsc  = ctx.engine_pop_tsc;
    event.answers_request = !ctx.answered;
    ctx.answered          = true;
    event.engine_push_tsc = __rdtsc();
    while (!out_ring_.push(event)) {}
}

void Orderbook::publish(const ITCH& msg) {
    if (!market_data_ring_.push(msg)) ++market_data_drops_;
}

ItchHeader Orderbook::itch_header(const Context& ctx, const ItchMessageType type) const {
    return ItchHeader{.message_type = type, .stock_locate = BOOK_STOCK_LOCATE, .tracking_number = 0,
                      .timestamp = ctx.itch_timestamp};
}
