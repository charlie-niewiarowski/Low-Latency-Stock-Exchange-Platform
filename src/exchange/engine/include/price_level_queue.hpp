//
// PriceLevelQueue (PLQ) — a single price level's FIFO of resting orders.
//
// A thin wrapper over an intrusive doubly-linked list of Order nodes. Preserves
// time priority (push at the back, pop from the front) while supporting O(1)
// removal of an arbitrary order (cancellation).
//

#ifndef PRICE_LEVEL_QUEUE_H
#define PRICE_LEVEL_QUEUE_H

#include "order.hpp"

#include <cstddef>

class PriceLevelQueue {
public:
    PriceLevelQueue() = default;

    PriceLevelQueue(const PriceLevelQueue&) = delete;
    PriceLevelQueue& operator=(const PriceLevelQueue&) = delete;
    PriceLevelQueue(PriceLevelQueue&&) = delete;
    PriceLevelQueue& operator=(PriceLevelQueue&&) = delete;

    ~PriceLevelQueue() = default;

    void push(Order *new_order) {
        if (!head) {
            head = new_order;
        }
        if (tail) {
            tail->set_next(new_order);
            new_order->set_prev(tail);
        }
        tail = new_order;
        ++size_;
    }

    Order* pop() {
        if (!head) return nullptr;
        Order *res{head};
        head = head->get_next();
        if (head) {
            head->set_prev(nullptr);
        } else {
            tail = nullptr;
        }
        --size_;
        return res;
    }

    Order *front() { return head; }

    void remove(const Order *to_remove) {
        Order *prev{to_remove->get_prev()}, *next{to_remove->get_next()};

        if (prev) prev->set_next(next);
        else      head = next;

        if (next) next->set_prev(prev);
        else      tail = prev;

        --size_;
    }

    bool empty() const { return head == nullptr; }
    size_t size() const { return size_; }
private:
    Order *head{nullptr}, *tail{nullptr};
    size_t size_{0};
};

#endif // PRICE_LEVEL_QUEUE_H
