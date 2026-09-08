//
// Created by charl on 11/25/2025.
//
// OrderRequest — the single input object to the Orderbook.
//
// Absorbs the former Command struct: it carries the message type and the
// assigned OrderId (the Engine assigns ids for NEW orders) alongside all the
// fields required to add/cancel/modify/match. The Engine builds one per inbound
// message and hands it to Orderbook::process().
//

#ifndef UNTITLED_TYPES_H
#define UNTITLED_TYPES_H

#include "order_types.hpp"
#include "communication_types.hpp"

class OrderRequest {
    MessageType message_type;
    OrderId order_id;
    ClientId client_id;
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;

    Timestamp recv_tsc;
    Timestamp server_push_tsc;
    Timestamp engine_pop_tsc;
public:
    OrderRequest(MessageType message_type, OrderId order_id, ClientId client_id,
                 Side side, OrderType type, Price price, Quantity quantity,
                 Timestamp recv_tsc = 0, Timestamp server_push_tsc = 0, Timestamp engine_pop_tsc = 0)
        : message_type(message_type), order_id(order_id), client_id(client_id), side(side),
          type(type), price(price), quantity(quantity),
          recv_tsc(recv_tsc), server_push_tsc(server_push_tsc), engine_pop_tsc(engine_pop_tsc) {}

    MessageType get_message_type()  const { return message_type;   }
    OrderId   get_order_id()        const { return order_id;        }
    ClientId  get_clientId()        const { return client_id;      }
    Side      get_side()            const { return side;            }
    OrderType get_type()            const { return type;            }
    Price     get_price()           const { return price;           }
    Quantity  get_quantity()        const { return quantity;        }
    Timestamp get_recv_tsc()        const { return recv_tsc;        }
    Timestamp get_server_push_tsc() const { return server_push_tsc; }
    Timestamp get_engine_in_tsc()   const { return engine_pop_tsc;   }
};

#endif //UNTITLED_TYPES_H
