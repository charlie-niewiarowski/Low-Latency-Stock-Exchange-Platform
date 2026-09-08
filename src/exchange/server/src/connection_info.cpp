//
// Created by cniew on 5/16/26.
//

#include "../include/connection_info.hpp"

OutboundMessage OutboundState::pop_outbound() {
    auto msg = staging_.pop();
    if (msg) --pending_count_;
    return msg.value_or(OutboundMessage{});
}