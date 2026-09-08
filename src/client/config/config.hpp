//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================

//===== data =====
#define DIAGNOSTICS 1
#define LOGGING 0

//===== market activity =====
// Valid price range — must match src/exchange/config/config.hpp's MIN_PRICE/
// MAX_PRICE: the exchange indexes its price ladder directly by (price -
// MIN_PRICE), so a generated price outside this range is a protocol error.
#define MIN_PRICE (1)
#define MAX_PRICE (1'000 * 100)

// GBM volatility per step: drives mid-price drift AND limit-price spread.
#define MID_PRICE_VOL 0.002

// Starting mid-price in integer ticks
#define MID_PRICE_INITIAL (100 * 100) // the first 100 is configurable the second is to represent the
                                      // decimal since $100.25 = (uint32_t)10025
#define MID_PRICE_UPDATE_N 16 // how often the mid-price is updated

// Lognormal quantity distribution
#define MEAN_QTY 100.0
#define QTY_VOL 0.8

//===== system config =====
#define EXCHANGE_HOST "127.0.0.1"
#define EXCHANGE_PORT  4000
#define CLIENT_CORE 4

// open-loop rate cap: aggregate orders / second across ALL clients.
// The orchestrator divides this evenly so each client's inter-arrival time is
// inter_arrival_ns = num_clients * 1e9 / EXPECTED_THROUGHPUT
// A client will not issue its next request until both (a) the previous
// response has been received AND (b) its inter-arrival slot has elapsed.
// Set to 0 to disable rate limiting (open-loop / go as fast as possible).
#define EXPECTED_THROUGHPUT 0

//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================
#define MAX_ACTIVE_ORDERS 32u

// Number of requests kept in-flight per connection
// Throughput ceiling approx = num_clients × PIPELINE_DEPTH / RTT
#define PIPELINE_DEPTH 32

#define CLIENT_EPOLL_BATCH 512

// Reconnect backoff, linear up to this ceiling
#define RECONNECT_DELAY_MS 1000

#endif // CLIENT_CONFIG_H
