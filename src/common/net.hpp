#pragma once

#include "common/protocol.hpp"

#include <netinet/in.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace steamu::net {

uint64_t now_us(); // CLOCK_MONOTONIC in microseconds

bool send_all(int fd, const void* data, size_t size);
bool recv_all(int fd, void* data, size_t size);

// Low-latency socket tuning: TCP_NODELAY, DSCP marking (Wi-Fi WMM queue
// selection) and SO_PRIORITY.
void tune_tcp(int fd);
void tune_udp(int fd);       // controller input: DSCP EF
void tune_udp_video(int fd); // video stream: DSCP AF41

std::string to_string(const sockaddr_in& addr);
bool parse_ipv4(const char* text, uint16_t port, sockaddr_in& out);

// Length-prefixed message framing over a connected TCP socket.
// send() is thread-safe; recv() must only be called from one thread.
class Channel {
public:
    explicit Channel(int fd) : fd_(fd) {}
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool send(proto::Msg type, const void* head, size_t head_size, const void* data = nullptr,
              size_t data_size = 0);
    bool recv(proto::Msg& type, std::vector<uint8_t>& payload);
    // Like send(), but drops the message instead of blocking when the peer
    // is not reading (small messages only). Returns false if dropped/failed.
    bool try_send(proto::Msg type, const void* data, size_t size);

    // Unblocks a pending recv() on another thread.
    void shutdown();
    int fd() const { return fd_; }

private:
    int fd_;
    std::mutex send_mutex_;
};

} // namespace steamu::net
