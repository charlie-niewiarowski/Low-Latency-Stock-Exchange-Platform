//
// Created by cniew on 5/19/26.
//

#ifndef TYPES_H
#define TYPES_H

#include <cstdint>

using Timestamp = uint64_t;
using ClientId = uint32_t;
// The client's own name for one of its orders (OUCH's UserRefNum): unique per
// client, so (ClientId, ClientOrderRef) names an order exchange-wide.
using ClientOrderRef = uint32_t;
enum class Side : uint8_t { BID, ASK };
enum class OrderType : uint8_t { MARKET, LIMIT };
enum class TimeInForce : uint8_t { DAY, IOC };
using OrderId = uint64_t;
using Price = uint32_t;
using Quantity = uint32_t;

#endif //TYPES_H
