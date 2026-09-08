//
// Created by cniew on 5/19/26.
//

#ifndef TYPES_H
#define TYPES_H

#include <cstdint>

using Timestamp = uint64_t;
using ClientId = uint32_t;
enum class Side : uint8_t { BID, ASK };
enum class OrderType : uint8_t { MARKET, LIMIT };
using OrderId = uint64_t;
using Price = uint32_t;
using Quantity = uint32_t;

#endif //TYPES_H
