//
// Created by cniew on 5/16/26.
//

#include "../include/connection_info.hpp"

OutboundMessage OutboundState::pop_outbound() {
    OutboundMessage msg{};
    if (staging_.pop(msg)) --pending_count_;
    return msg;
}