//
// OrderPool — the single owner of every Order object.
//
// The OrderMap (OrderId -> Order*) and the PriceLevelQueues store only
// non-owning pointers into this pool. Centralising ownership here lets us:
//   * avoid per-order heap allocations on the hot path,
//   * reuse memory deterministically, and
//   * keep Order objects contiguous for cache locality.
//
// The arena is a fixed-capacity contiguous block of Orders. A free list (a
// stack of pointers to unused slots) gives O(1) allocate() and deallocate().
//

#ifndef ORDER_POOL_H
#define ORDER_POOL_H

#include "order.hpp"
#include "order_request.hpp"
#include "order_types.hpp"

#include <cstddef>
#include <vector>

class OrderPool {
public:
    explicit OrderPool(const size_t capacity) : arena_(capacity) {
        free_list_.reserve(capacity);
        // Push in reverse so the earliest allocations hand out the lowest
        // addresses, keeping the working set near the front of the arena.
        for (size_t i = capacity; i-- > 0;) free_list_.push_back(&arena_[i]);
    }

    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&)                 = delete;
    OrderPool& operator=(OrderPool&&)      = delete;

    ~OrderPool() = default;

    // Hands out a reusable slot initialised as the requested Order.
    // Returns nullptr when the pool is exhausted.
    Order* allocate(const OrderId id, const OrderRequest& request) {
        if (free_list_.empty()) return nullptr;
        Order* slot = free_list_.back();
        free_list_.pop_back();
        *slot = Order{id, request};   // resets prev_/next_ to nullptr as well
        return slot;
    }

    // Returns a slot to the pool for reuse. The Order must have been obtained
    // from allocate() and already unlinked from its price level.
    void deallocate(Order* order) {
        free_list_.push_back(order);
    }

    [[nodiscard]] size_t capacity()  const { return arena_.size(); }
    [[nodiscard]] size_t available() const { return free_list_.size(); }
    [[nodiscard]] size_t in_use()    const { return arena_.size() - free_list_.size(); }

private:
    std::vector<Order>  arena_;      // owns every Order object
    std::vector<Order*> free_list_;  // stack of reusable slots
};

#endif // ORDER_POOL_H
