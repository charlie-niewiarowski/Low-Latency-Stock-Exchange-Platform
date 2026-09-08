//
// Shared test infrastructure: fake engine thread + server thread + client helpers.
//

#ifndef SERVER_FIXTURE_H
#define SERVER_FIXTURE_H

#include "server.hpp"
#include "protocol.hpp"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

//=============================================================================
// Fake Engine
//=============================================================================

// Mimics the matching engine: pops every InboundMessage from the inbound ring
// and pushes a SUCCESS OutboundMessage back.  NEW orders get a fresh order_id;
// CANCEL / MODIFY echo the order_id from the request.

class FakeEngine {
public:
    FakeEngine(InboundRing& in, OutboundRing& out) : in_(in), out_(out) {}

    void start() {
        running_ = true;
        thread_ = std::thread([this]{ run(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    ~FakeEngine() { stop(); }

private:
    void run() {
        while (running_) {
            if (auto in_msg = in_.pop()) {
                OutboundMessage out_msg{};
                out_msg.client_id    = in_msg->client_id;
                out_msg.order_id     = (in_msg->message_type == MessageType::NEW)
                                           ? next_order_id_++ : in_msg->order_id;
                out_msg.message_type = in_msg->message_type;
                out_msg.status       = Status::SUCCESS;

                while (!out_.push(out_msg) && running_) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    InboundRing& in_;
    OutboundRing& out_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_order_id_{1};
};

//=============================================================================
// Server Fixture
//=============================================================================

struct ServerFixture {
    InboundRing   in_ring{64};
    OutboundRing  out_ring{64};
    std::atomic<bool> stop_flag{false};
    Server        server{in_ring, out_ring, stop_flag};
    FakeEngine    engine{in_ring, out_ring};
    std::thread   server_thread;

    // Starts the server + fake engine and waits until the server is ready to accept.
    void start() {
        server_thread = std::thread([this]{ server.run(); });
        engine.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    void stop() {
        engine.stop();
        server.stop();
        if (server_thread.joinable()) server_thread.join();
    }

    ~ServerFixture() { stop(); }
};

//=============================================================================
// Client Helpers
//=============================================================================

// Opens a blocking TCP connection to the server.  Returns -1 on failure.
static int connect_client() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(4000);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        ::close(fd);
        return -1;
    }

    // 2-second receive timeout so tests don't hang forever on unexpected failure.
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// Sends "EXCHANGE\n" + the InboundMessage binary payload.
static void send_request(int fd, const InboundMessage& msg) {
    char buf[INBOUND_BSIZE];
    size_t off = static_cast<size_t>(snprintf(buf, sizeof(buf), "EXCHANGE\n"));
    memcpy(buf + off, &msg, sizeof(msg));
    off += sizeof(msg);

    size_t sent = 0;
    while (sent < off) {
        ssize_t n = ::write(fd, buf + sent, off - sent);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// Sends raw bytes (used to inject malformed requests).
static void send_raw(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

//=============================================================================
// Response Parsing
//=============================================================================

struct OkResponse  { bool valid; ClientId client_id; OrderId order_id; };
struct ErrResponse { bool valid; std::string message; };

static OkResponse read_ok_response(int fd) {
    char buf[OUTBOUND_BSIZE + 1]{};
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return {false, 0, 0};

    const char* header = "EXCHANGE\nOK\n";
    size_t hlen = strlen(header);
    if (static_cast<size_t>(n) < hlen + sizeof(ClientId) + sizeof(OrderId)) return {false, 0, 0};
    if (memcmp(buf, header, hlen) != 0) return {false, 0, 0};

    ClientId cid{}; OrderId oid{};
    memcpy(&cid, buf + hlen, sizeof(cid));
    memcpy(&oid, buf + hlen + sizeof(cid), sizeof(oid));
    return {true, cid, oid};
}

static ErrResponse read_error_response(int fd) {
    char buf[OUTBOUND_BSIZE + 1]{};
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return {false, ""};

    const char* header = "EXCHANGE\nERROR\n";
    size_t hlen = strlen(header);
    if (static_cast<size_t>(n) < hlen) return {false, ""};
    if (memcmp(buf, header, hlen) != 0) return {false, ""};

    return {true, std::string(buf + hlen, static_cast<size_t>(n) - hlen)};
}

#endif // SERVER_FIXTURE_H