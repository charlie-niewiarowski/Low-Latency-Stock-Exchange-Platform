//
// Created by cniew on 9/11/26.
//

#include "server.hpp"
#include <gtest/gtest.h>

class ServerTest : public testing::Test {
protected:
    ServerTest() : server_(
            std::make_shared<RingBuffer<OUCH>>(RINGBUF_SIZE),
                std::make_shared<RingBuffer<OUCH>>(RINGBUF_SIZE)) {
        server_.run();
    }

    void Connect() {
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(socket_fd_, 0);

        sockaddr_in server_address{};
        memset(&server_address, 0, sizeof(server_address));

        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(8080);

        ASSERT_GE(inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr), 1);

        ASSERT_GE(connect(socket_fd_, (struct sockaddr*)&server_address, sizeof(server_address)), 0);
    }

    void Disconnect() {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    Server server_;
    int socket_fd_ = -1;
};

TEST_F(ServerTest, test_receives_packets) {
    Connect();

    const char *buffer = "some random bullshit";
    ASSERT_GE(write(socket_fd_, buffer, strlen(buffer)), strlen(buffer));

    Disconnect();
}