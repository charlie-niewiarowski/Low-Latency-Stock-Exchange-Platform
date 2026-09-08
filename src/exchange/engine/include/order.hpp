//
// Order — the fundamental order object stored in the book.
//
// Intrusive doubly-linked-list node (prev_/next_) so a PriceLevelQueue can
// insert, pop, and remove it in O(1). Owned by the OrderPool; referenced by the
// OrderMap and the price-level queues.
//

#ifndef ORDER_H
#define ORDER_H

#include "order_request.hpp"
#include "order_types.hpp"
#include "config.hpp"

#include <cstdint>
#include <stdexcept>

class Order { // main data variable
public:
    // Default-constructed Orders exist only as unused slots inside the OrderPool
    // arena; allocate() overwrites every field before the Order is handed out.
    Order() = default;

    Order(const OrderId id, const OrderRequest &order_request) {
        id_ = id;
        client_id_ = order_request.get_clientId();
        side_ = order_request.get_side();
        order_type_ = order_request.get_type();
        price_ = order_request.get_price();
        initial_quantity_ = order_request.get_quantity();
        remaining_quantity_ = order_request.get_quantity();
        recv_tsc = order_request.get_recv_tsc();
        #if DIAGNOSTICS
        server_push_tsc = order_request.get_server_push_tsc();
        engine_pop_tsc = order_request.get_engine_in_tsc();
        #endif
    }

    bool is_filled() const { return remaining_quantity_ == 0; }
    void fill(const Quantity quantity) {
        if (quantity > remaining_quantity_) {
            throw std::logic_error("Filling quantity larger than remaining quantity");
        }

        remaining_quantity_ -= quantity;
    }

    void set_next(Order *next) { next_ = next; }
    [[nodiscard]] Order *get_next() const { return next_; }

    void set_prev(Order *prev) {prev_ = prev; }
    [[nodiscard]] Order *get_prev() const { return prev_; }

    friend class Orderbook;
private:
    Timestamp recv_tsc;           // 8 bytes  @ 0
    #if DIAGNOSTICS
    Timestamp server_push_tsc;    // 8 bytes  @ 8
    Timestamp engine_pop_tsc;     // 8 bytes  @ 16
    #endif
    OrderId id_;                  // 8 bytes  @ 8/24
    Order *prev_{nullptr};        // 8 bytes  @ 16/32
    Order *next_{nullptr};        // 8 bytes  @ 24/40
    ClientId client_id_;          // 4 bytes  @ 28/44
    Price price_;                 // 4 bytes  @ 32/48
    Quantity initial_quantity_;   // 4 bytes  @ 32/48
    Quantity remaining_quantity_; // 4 bytes  @ 36/52
    Side side_;                   // 1 byte   @ 40/56
    OrderType order_type_;        // 1 byte   @ 41/57
    uint8_t padding[7];           // 6 bytes  @ 42/58
}; // 48/64 bytes (sizeof verified by compiler)

#endif // ORDER_H
