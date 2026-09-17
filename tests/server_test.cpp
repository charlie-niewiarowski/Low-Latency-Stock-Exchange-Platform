//
// Created by cniew on 9/11/26.
//

#include "server.hpp"

#include <array>
#include <string_view>
#include <thread>
#include <gtest/gtest.h>

class ServerTest : public testing::Test {
protected:
    ServerTest() : server_(
            std::make_shared<RingBuffer<OUCH>>(RINGBUF_SIZE),
                std::make_shared<RingBuffer<OUCH>>(RINGBUF_SIZE)),
            client_tx_queue_(server_.dpdk_port_.claim_tx_queue()),
            server_thread_(&Server::run, &server_) {}

    ~ServerTest() override {
        server_.shutdown_.store(true);
    }

    uint16_t SendPacket(std::string_view payload) const {
        dpdk::packet pkt = server_.dpdk_port_.get_pool()->get();
        if (!pkt) {
            return 0;
        }

        const auto *bytes = reinterpret_cast<const std::byte *>(payload.data());
        if (pkt.append(dpdk::packet::Payload(bytes, payload.size())) == nullptr) {
            return 0;
        }

        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return client_tx_queue_->send_burst(pkts);
    }

    Server server_;
    std::shared_ptr<dpdk::tx_queue> client_tx_queue_;
    std::jthread server_thread_;
};

TEST_F(ServerTest, test_receives_packets) {
    ASSERT_TRUE(client_tx_queue_);

    constexpr std::string_view payload = "some random bullshit";
    ASSERT_EQ(SendPacket(payload), 1);

    const std::chrono::seconds duration{2};
    const auto end = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < end) {
        // do nothing
    }
}
