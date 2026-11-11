#include "common/net.hpp"

#include "common/log.hpp"

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace steamu::net {

uint64_t now_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec) / 1000u;
}

bool send_all(int fd, const void* data, size_t size)
{
    auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= size_t(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t size)
{
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0) {
        ssize_t n = ::recv(fd, p, size, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= size_t(n);
    }
    return true;
}

static void set_priority(int fd, int tos, int prio)
{
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
}

// Small, sparse, latency-critical packets (controller input, control
// messages): DSCP EF (46) -> Wi-Fi WMM "voice" queue.
static void set_common_priority(int fd)
{
    set_priority(fd, 0xB8, 6);
}

void tune_tcp(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    // Detect a dead Wi-Fi link within a few seconds instead of minutes.
    int idle = 2, intvl = 1, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    unsigned timeout_ms = 5000;
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    set_common_priority(fd);
}

void tune_udp(int fd)
{
    set_common_priority(fd);
}

void tune_udp_video(int fd)
{
    // Bulk video must NOT go in the voice queue: there it cannot be
    // aggregated and thousands of packets/s monopolise the router's radio.
    // DSCP AF41 (34) -> WMM "video" queue, like Moonlight/Sunshine.
    set_priority(fd, 0x88, 5);
}

std::string to_string(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool parse_ipv4(const char* text, uint16_t port, sockaddr_in& out)
{
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, text, &out.sin_addr) == 1;
}

Channel::~Channel()
{
    if (fd_ >= 0) ::close(fd_);
}

bool Channel::send(proto::Msg type, const void* head, size_t head_size, const void* data,
                   size_t data_size)
{
    proto::MsgHeader hdr{};
    hdr.type = uint16_t(type);
    hdr.size = uint32_t(head_size + data_size);

    iovec iov[3] = {
        {&hdr, sizeof(hdr)},
        {const_cast<void*>(head), head_size},
        {const_cast<void*>(data), data_size},
    };

    std::lock_guard lock(send_mutex_);
    size_t index = 0;
    while (index < 3) {
        msghdr msg{};
        msg.msg_iov = iov + index;
        msg.msg_iovlen = 3 - index;
        ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        // Advance past what was written (handles partial writes).
        size_t left = size_t(n);
        while (index < 3 && left >= iov[index].iov_len) {
            left -= iov[index].iov_len;
            ++index;
        }
        if (index < 3) {
            iov[index].iov_base = static_cast<uint8_t*>(iov[index].iov_base) + left;
            iov[index].iov_len -= left;
        }
    }
    return true;
}

bool Channel::try_send(proto::Msg type, const void* data, size_t size)
{
    uint8_t buf[sizeof(proto::MsgHeader) + 256];
    if (size > 256) return false;
    proto::MsgHeader hdr{};
    hdr.type = uint16_t(type);
    hdr.size = uint32_t(size);
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), data, size);
    const size_t total = sizeof(hdr) + size;

    std::lock_guard lock(send_mutex_);
    ssize_t n = ::send(fd_, buf, total, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) return false; // full (EAGAIN) or broken: drop
    if (size_t(n) < total) {
        // Partially queued: the rest must follow or the stream breaks.
        const uint8_t* rest = buf + n;
        size_t left = total - size_t(n);
        while (left > 0) {
            ssize_t m = ::send(fd_, rest, left, MSG_NOSIGNAL);
            if (m < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            rest += m;
            left -= size_t(m);
        }
    }
    return true;
}

bool Channel::recv(proto::Msg& type, std::vector<uint8_t>& payload)
{
    proto::MsgHeader hdr{};
    if (!recv_all(fd_, &hdr, sizeof(hdr))) return false;
    if (hdr.size > proto::kMaxMessage) {
        LOGE("control message too large (%u bytes), dropping connection", hdr.size);
        return false;
    }
    type = proto::Msg(hdr.type);
    payload.resize(hdr.size);
    return hdr.size == 0 || recv_all(fd_, payload.data(), hdr.size);
}

void Channel::shutdown()
{
    ::shutdown(fd_, SHUT_RDWR);
}

} // namespace steamu::net
