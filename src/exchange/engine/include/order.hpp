//
// Order — the fundamental order object stored in the book.
//
// Intrusive doubly-linked-list node (prev_/next_) so a PriceLevelQueue can
// insert, pop, and remove it in O(1). Owned by the OrderPool; referenced by the
// OrderMap and the price-level queues. Only resting orders exist as Orders —
// an incoming order matches against the book before (and unless) it rests.
//

#ifndef ORDER_H
#define ORDER_H

#include "order_types.hpp"

#include <cstdint>

class Order { // main data variable
public:
    // Default-constructed Orders exist only as unused slots inside the OrderPool
    // arena; allocate() overwrites every field before the Order is handed out.
    Order() = default;

    Order(const OrderId id, const ClientId client_id, const Side side, const Price price,
          const Quantity open_quantity, const Quantity executed_quantity)
        : id_(id), client_id_(client_id), price_(price), open_quantity_(open_quantity),
          executed_quantity_(executed_quantity), side_(side) {}

    [[nodiscard]] bool is_filled() const { return open_quantity_ == 0; }

    void set_next(Order *next) { next_ = next; }
    [[nodiscard]] Order *get_next() const { return next_; }

    void set_prev(Order *prev) {prev_ = prev; }
    [[nodiscard]] Order *get_prev() const { return prev_; }

    friend class Orderbook;
private:
    OrderId id_;                  // 8 bytes  @ 0
    Order *prev_{nullptr};        // 8 bytes  @ 8
    Order *next_{nullptr};        // 8 bytes  @ 16
    ClientId client_id_;          // 4 bytes  @ 24
    Price price_;                 // 4 bytes  @ 28
    Quantity open_quantity_;      // 4 bytes  @ 32  |  shares still on the book
    Quantity executed_quantity_;  // 4 bytes  @ 36  |  executed across the whole order/replace chain
    Side side_;                   // 1 byte   @ 40
    uint8_t padding[7];           // 7 bytes  @ 41
}; // 48 bytes (sizeof verified by compiler)
static_assert(sizeof(Order) == 48);

#endif // ORDER_H
