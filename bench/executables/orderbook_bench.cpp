//
// Standalone microbenchmark for Orderbook::process() — the matching engine's
// hot path — driven directly, with no Engine/Server/ring-thread involvement.
//
// This is a workload generator only: it issues NEW/CANCEL/MODIFY requests
// against a real Orderbook at a fixed, seeded pace and exists to be run under
// `perf stat`, driven by bench/scripts/orderbook_perf.py (a thin wrapper over
// bench/scripts/perf_on_executable.py's generic perf-stat runner). The binary
// itself just prints a sanity summary so a run can be eyeballed for
// plausibility (order counts, level counts, op mix).
//
// Doesn't touch the TCP exchange/client pair the rest of bench/ drives.
// Deliberately dependency-free beyond orderbook.hpp/.cpp and infra/perf.hpp
// so it builds without FetchContent (no hdr_histogram, no dpdk).
//

#include "orderbook.hpp"
#include "perf.hpp"

#include "args.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

struct ResidentOrder {
    OrderId  id;
    ClientId client_id;
    Side     side;
};

// Fixed-capacity ring of recently-submitted resting orders, sampled as CANCEL/
// MODIFY targets. Entries aren't removed when the order is later filled or
// canceled elsewhere — a stale target just makes process() take its FAILURE
// path (checked safely via orders_.find() before any orders_.at()), which is
// realistic traffic in its own right (late/duplicate cancels happen on real
// exchanges) and costs nothing extra to model.
class ResidentPool {
public:
    explicit ResidentPool(size_t capacity) : buf_(capacity) {}

    void push(const ResidentOrder& o) {
        buf_[next_ % buf_.size()] = o;
        ++next_;
        if (count_ < buf_.size()) ++count_;
    }

    const ResidentOrder& sample(std::mt19937_64& rng) const {
        std::uniform_int_distribution<size_t> d(0, count_ - 1);
        return buf_[d(rng)];
    }

    bool empty() const { return count_ == 0; }

private:
    std::vector<ResidentOrder> buf_;
    size_t next_{0};
    size_t count_{0};
};

enum class Op : uint8_t { NEW_LIMIT, NEW_MARKET, CANCEL, MODIFY };

Price clamp_price(int64_t p) {
    if (p < MIN_PRICE) return MIN_PRICE;
    if (p > MAX_PRICE) return MAX_PRICE;
    return static_cast<Price>(p);
}

} // namespace

int main(int argc, char** argv) {
    using bench_harness::arg_u64;
    using bench_harness::arg_i64;

    const uint64_t total_ops = arg_u64(argc, argv, "--ops", 30'000'000);
    const uint64_t seed      = arg_u64(argc, argv, "--seed", 42);
    const int64_t  core      = arg_i64(argc, argv, "--core", -1);
    const int64_t  depth_max = arg_i64(argc, argv, "--depth-max", 50);
    // Op-mix, in percent (cumulative thresholds below); MODIFY takes whatever's
    // left. Defaults reproduce the original fixed 55/10/20/15 blended mix.
    // Override to isolate one op type, e.g. --pct-new-limit 100 --pct-new-market 0
    // --pct-cancel 0 --aggressive-pct 0 for an add-only (never-crosses) run.
    const int64_t  pct_new_limit  = arg_i64(argc, argv, "--pct-new-limit", 55);
    const int64_t  pct_new_market = arg_i64(argc, argv, "--pct-new-market", 10);
    const int64_t  pct_cancel     = arg_i64(argc, argv, "--pct-cancel", 20);
    const int64_t  aggressive_pct = arg_i64(argc, argv, "--aggressive-pct", 15);
    // Untimed-in-intent (still inside the same perf-measured process) prefix of
    // pure resting NEW_LIMIT orders, so a --pct-cancel/--pct-modify-heavy run
    // has real targets instead of mostly missing on an empty book.
    const uint64_t seed_ops   = arg_u64(argc, argv, "--seed-ops", 0);

    if (core >= 0) pin_to_core(static_cast<int>(core));

    OutboundRing out_ring(1u << 16);
    Orderbook book(out_ring);

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<uint32_t> client_dist(1, 2000);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 500);
    std::uniform_int_distribution<int64_t> depth_dist(1, depth_max);   // ticks into the book
    std::uniform_int_distribution<int64_t> walk_dist(-3, 3);

    ResidentPool resting(1u << 20);

    int64_t mid = 100 * 100;              // matches client config's MID_PRICE_INITIAL
    const int64_t half_spread = 5;

    OrderId next_id = 1;
    uint64_t new_limit = 0, new_market = 0, cancels = 0, cancel_hits = 0,
             modifies = 0, modify_hits = 0;

    const double t1 = pct_new_limit / 100.0;
    const double t2 = t1 + pct_new_market / 100.0;
    const double t3 = t2 + pct_cancel / 100.0;
    const double aggressive_frac = aggressive_pct / 100.0;

    for (uint64_t i = 0; i < seed_ops + total_ops; ++i) {
        if ((i & 4095) == 0) {
            mid = clamp_price(mid + walk_dist(rng) * 4);
        }

        const bool seeding = i < seed_ops;
        const double r = unit(rng);
        Op op;
        if (seeding)       op = Op::NEW_LIMIT;
        else if (r < t1)   op = Op::NEW_LIMIT;
        else if (r < t2)   op = Op::NEW_MARKET;
        else if (r < t3)   op = Op::CANCEL;
        else               op = Op::MODIFY;

        const ClientId cid = client_dist(rng);

        switch (op) {
            case Op::NEW_LIMIT: {
                const Side side = (rng() & 1) ? Side::BID : Side::ASK;
                const bool aggressive = !seeding && unit(rng) < aggressive_frac; // crosses the spread -> match
                const int64_t depth = depth_dist(rng);
                Price price;
                if (side == Side::BID) {
                    price = clamp_price(aggressive ? mid + half_spread + 1
                                                    : mid - half_spread - depth);
                } else {
                    price = clamp_price(aggressive ? mid - half_spread - 1
                                                    : mid + half_spread + depth);
                }
                const Quantity qty = qty_dist(rng);
                const OrderId id = next_id++;

                OrderRequest req(MessageType::NEW, id, cid, side, OrderType::LIMIT, price, qty);
                book.process(req);
                resting.push({id, cid, side});
                ++new_limit;
                break;
            }
            case Op::NEW_MARKET: {
                const Side side = (rng() & 1) ? Side::BID : Side::ASK;
                const Quantity qty = qty_dist(rng);
                const OrderId id = next_id++;

                OrderRequest req(MessageType::NEW, id, cid, side, OrderType::MARKET, 0, qty);
                book.process(req);
                ++new_market;
                break;
            }
            case Op::CANCEL: {
                ++cancels;
                if (resting.empty()) break;
                const ResidentOrder& target = resting.sample(rng);
                OrderRequest req(MessageType::CANCEL, target.id, target.client_id,
                                  Side::BID, OrderType::LIMIT, 0, 0);
                if (book.process(req).status == Status::SUCCESS) ++cancel_hits;
                break;
            }
            case Op::MODIFY: {
                ++modifies;
                if (resting.empty()) break;
                const ResidentOrder& target = resting.sample(rng);
                const int64_t depth = depth_dist(rng);
                const Price price = clamp_price(target.side == Side::BID
                                                     ? mid - half_spread - depth
                                                     : mid + half_spread + depth);
                const Quantity qty = qty_dist(rng);
                OrderRequest req(MessageType::MODIFY, target.id, target.client_id,
                                  target.side, OrderType::LIMIT, price, qty);
                if (book.process(req).status == Status::SUCCESS) ++modify_hits;
                break;
            }
        }

        // out_ring_'s pushOut() spins if the ring is ever full, so keep it
        // drained — a single process() call can emit several MATCH messages
        // during a level sweep.
        while (out_ring.pop()) {}
    }

    std::printf("seed_ops=%llu ops=%llu new_limit=%llu new_market=%llu "
                "cancel=%llu/%llu (hit) modify=%llu/%llu (hit)\n",
                static_cast<unsigned long long>(seed_ops),
                static_cast<unsigned long long>(total_ops),
                static_cast<unsigned long long>(new_limit),
                static_cast<unsigned long long>(new_market),
                static_cast<unsigned long long>(cancel_hits),
                static_cast<unsigned long long>(cancels),
                static_cast<unsigned long long>(modify_hits),
                static_cast<unsigned long long>(modifies));
    std::printf("final order_count=%zu bid_levels=%zu ask_levels=%zu\n",
                book.order_count(), book.bid_level_count(), book.ask_level_count());

    return 0;
}
