#include "host/usbip_device.hpp"

#include "common/log.hpp"
#include "common/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace steamu::host {

namespace {

const char* const kVhciDir = "/sys/devices/platform/vhci_hcd.0";

// USB/IP commands (Documentation/usb/usbip_protocol.rst). Big-endian.
constexpr uint32_t kCmdSubmit = 1;
constexpr uint32_t kCmdUnlink = 2;
constexpr uint32_t kRetSubmit = 3;
constexpr uint32_t kRetUnlink = 4;
constexpr size_t kHeaderSize = 48;

// USB request bits
constexpr uint8_t kReqGetStatus = 0x00;
constexpr uint8_t kReqClearFeature = 0x01;
constexpr uint8_t kReqSetFeature = 0x03;
constexpr uint8_t kReqSetAddress = 0x05;
constexpr uint8_t kReqGetDescriptor = 0x06;
constexpr uint8_t kReqGetConfiguration = 0x08;
constexpr uint8_t kReqSetConfiguration = 0x09;
constexpr uint8_t kReqGetInterface = 0x0A;
constexpr uint8_t kReqSetInterface = 0x0B;
// HID class requests
constexpr uint8_t kHidGetReport = 0x01;
constexpr uint8_t kHidGetIdle = 0x02;
constexpr uint8_t kHidGetProtocol = 0x03;
constexpr uint8_t kHidSetReport = 0x09;
constexpr uint8_t kHidSetIdle = 0x0A;
constexpr uint8_t kHidSetProtocol = 0x0B;

uint32_t get_be32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return ntohl(v);
}

void put_be32(uint8_t* p, uint32_t v)
{
    v = htonl(v);
    std::memcpy(p, &v, 4);
}

// USB report type (1 input, 2 output, 3 feature) -> uhid report type.
uint8_t to_uhid_rtype(uint8_t usb_type)
{
    switch (usb_type) {
    case 1: return 2;
    case 2: return 1;
    default: return 0;
    }
}

int find_free_port(uint8_t speed)
{
    // Lines look like: "hs  0000 004 000 00000000 000000 0-0"
    // hub "hs" serves low/full/high speed, "ss" super speed; sta 4 = free.
    std::ifstream f(std::string(kVhciDir) + "/status");
    std::string line;
    const char* want_hub = speed >= 5 ? "ss" : "hs";
    while (std::getline(f, line)) {
        char hub[4] = {};
        int port = -1, status = -1;
        if (std::sscanf(line.c_str(), "%3s %d %d", hub, &port, &status) == 3 &&
            std::strcmp(hub, want_hub) == 0 && status == 4)
            return port;
    }
    return -1;
}

bool write_sysfs(const std::string& path, const std::string& value)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, value.data(), value.size());
    int saved = errno;
    ::close(fd);
    errno = saved;
    return n == ssize_t(value.size());
}

// Loopback TCP pair, the transport usbip itself uses.
bool make_tcp_pair(int fds[2])
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listener, 1) < 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        if (listener >= 0) close(listener);
        return false;
    }
    fds[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fds[0] < 0 || connect(fds[0], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listener);
        if (fds[0] >= 0) close(fds[0]);
        return false;
    }
    fds[1] = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    close(listener);
    if (fds[1] < 0) {
        close(fds[0]);
        return false;
    }
    int one = 1;
    setsockopt(fds[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fds[1], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
}

} // namespace

UsbipDevice::~UsbipDevice()
{
    destroy();
}

bool UsbipDevice::create(UsbIdentity identity, ControllerCallbacks callbacks)
{
    destroy();
    if (!identity.finalize()) return false;
    id_ = std::move(identity);
    callbacks_ = std::move(callbacks);

    if (access(kVhciDir, F_OK) != 0) {
        LOGE("USB/IP virtual host controller not loaded. Run: sudo modprobe vhci-hcd "
             "(and see README for making it permanent)");
        return false;
    }
    const int port = find_free_port(id_.speed);
    if (port < 0) {
        LOGE("no free vhci-hcd port");
        return false;
    }

    // fds[0] is ours, fds[1] is handed to the kernel. Try a Unix socket pair
    // first (no TCP overhead), loopback TCP if the kernel rejects it.
    for (int attempt = 0; attempt < 2; ++attempt) {
        int fds[2] = {-1, -1};
        const bool ok = attempt == 0
                            ? socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0
                            : make_tcp_pair(fds);
        if (!ok) continue;

        const uint32_t devid = (1u << 16) | uint32_t(port + 2);
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "%d %d %u %u", port, fds[1], devid, unsigned(id_.speed));
        const bool attached = write_sysfs(std::string(kVhciDir) + "/attach", cmd);
        const int err = errno;
        close(fds[1]); // the kernel holds its own reference now
        if (attached) {
            sock_ = fds[0];
            port_ = port;
            break;
        }
        close(fds[0]);
        if (err == EACCES || err == EPERM) {
            LOGE("no permission to attach USB devices (%s/attach). Install "
                 "packaging/60-steamu.rules and reload udev, see README",
                 kVhciDir);
            return false;
        }
        LOGD("vhci attach with %s socket failed: %s", attempt == 0 ? "unix" : "tcp",
             strerror(err));
        if (attempt == 1) {
            LOGE("vhci-hcd attach failed: %s", strerror(err));
            return false;
        }
    }

    reader_ = std::thread([this] { reader_loop(); });
    LOGI("virtual USB controller attached on vhci port %d: '%s %s' %02X%02X:%02X%02X, gamepad on "
         "interface %u",
         port_, id_.manufacturer.c_str(), id_.product.c_str(), id_.device[9], id_.device[8],
         id_.device[11], id_.device[10], id_.gamepad_interface);
    return true;
}

void UsbipDevice::destroy()
{
    int sock;
    {
        std::lock_guard lock(mutex_);
        sock = sock_;
        if (sock < 0) return;
    }
    write_sysfs(std::string(kVhciDir) + "/detach", std::to_string(port_));
    ::shutdown(sock, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard lock(mutex_);
        ::close(sock_);
        sock_ = -1;
        port_ = -1;
        in_urbs_.clear();
        queued_input_.clear();
        parked_.clear();
        control_.clear();
        reading_ = false;
    }
    LOGI("virtual USB controller detached");
}

void UsbipDevice::complete_locked(uint32_t seqnum, int32_t status, bool in, const uint8_t* data,
                                  size_t length)
{
    if (sock_ < 0) return;
    uint8_t buf[kHeaderSize + 1024] = {};
    if (!in || status != 0) {
        if (in) length = 0;
        data = nullptr;
    }
    length = std::min<size_t>(length, 1024);
    put_be32(buf + 0, kRetSubmit);
    put_be32(buf + 4, seqnum);
    put_be32(buf + 20, uint32_t(status));
    put_be32(buf + 24, uint32_t(length)); // actual_length
    if (data && length) std::memcpy(buf + kHeaderSize, data, length);
    net::send_all(sock_, buf, kHeaderSize + (data ? length : 0));
}

void UsbipDevice::complete(uint32_t seqnum, int32_t status, bool in, const uint8_t* data,
                           size_t length)
{
    std::lock_guard lock(mutex_);
    complete_locked(seqnum, status, in, data, length);
}

bool UsbipDevice::send_input(const uint8_t* data, size_t size)
{
    std::lock_guard lock(mutex_);
    if (sock_ < 0) return false;
    if (!in_urbs_.empty()) {
        const InUrb urb = in_urbs_.front();
        in_urbs_.pop_front();
        complete_locked(urb.seqnum, 0, true, data, std::min<size_t>(size, urb.length));
        return true;
    }
    // Nobody is polling right now (normally the driver resubmits within
    // microseconds). Keep a few so short button taps are not lost.
    if (queued_input_.size() >= 4) queued_input_.pop_front();
    queued_input_.emplace_back(data, data + size);
    return true;
}

void UsbipDevice::reply_get_report(uint32_t id, int err, const uint8_t* data, size_t size)
{
    std::lock_guard lock(mutex_);
    auto it = control_.find(id);
    if (it == control_.end()) return; // unlinked meanwhile
    const ControlRequest req = it->second;
    control_.erase(it);
    if (err) {
        complete_locked(req.seqnum, -EPIPE, true, nullptr, 0);
        return;
    }
    // hidraw data starts with the report number; on the USB wire it is only
    // present for numbered reports.
    if (req.rnum == 0 && size > 0) {
        ++data;
        --size;
    }
    complete_locked(req.seqnum, 0, true, data, std::min<size_t>(size, req.length));
}

void UsbipDevice::reply_set_report(uint32_t id, int err)
{
    std::lock_guard lock(mutex_);
    auto it = control_.find(id);
    if (it == control_.end()) return;
    const ControlRequest req = it->second;
    control_.erase(it);
    complete_locked(req.seqnum, err ? -EPIPE : 0, false, nullptr, err ? 0 : req.length);
}

void UsbipDevice::handle_unlink(uint32_t seqnum, uint32_t victim)
{
    std::lock_guard lock(mutex_);
    int32_t status = 0; // 0 = already completed
    auto in = std::find_if(in_urbs_.begin(), in_urbs_.end(),
                           [&](const InUrb& u) { return u.seqnum == victim; });
    if (in != in_urbs_.end()) {
        in_urbs_.erase(in);
        status = -ECONNRESET;
    } else if (parked_.erase(victim)) {
        status = -ECONNRESET;
    } else {
        for (auto it = control_.begin(); it != control_.end(); ++it) {
            if (it->second.seqnum == victim) {
                control_.erase(it);
                status = -ECONNRESET;
                break;
            }
        }
    }
    uint8_t buf[kHeaderSize] = {};
    put_be32(buf + 0, kRetUnlink);
    put_be32(buf + 4, seqnum);
    put_be32(buf + 20, uint32_t(status));
    if (sock_ >= 0) net::send_all(sock_, buf, sizeof(buf));
}

void UsbipDevice::handle_control(uint32_t seqnum, const uint8_t* setup,
                                 const std::vector<uint8_t>& out)
{
    const uint8_t bm = setup[0], req = setup[1];
    const uint16_t value = uint16_t(setup[2] | setup[3] << 8);
    const uint16_t index = uint16_t(setup[4] | setup[5] << 8);
    const uint16_t length = uint16_t(setup[6] | setup[7] << 8);
    const bool in = bm & 0x80;
    const uint8_t type = (bm >> 5) & 0x03;   // 0 standard, 1 class
    const uint8_t recipient = bm & 0x1F;     // 0 device, 1 interface, 2 endpoint

    auto reply = [&](const std::vector<uint8_t>& d) {
        complete(seqnum, 0, true, d.data(), std::min<size_t>(d.size(), length));
    };
    auto ack = [&] { complete(seqnum, 0, in, nullptr, in ? 0 : out.size()); };
    auto stall = [&] {
        LOGD("usb: stall bm=%02x req=%02x value=%04x index=%04x len=%u", bm, req, value, index,
             length);
        complete(seqnum, -EPIPE, in, nullptr, 0);
    };

    if (type == 0) { // standard
        switch (req) {
        case kReqGetDescriptor: {
            const uint8_t dtype = uint8_t(value >> 8), dindex = uint8_t(value);
            std::vector<uint8_t> d;
            if (recipient == 0 && dtype == 0x01) d = id_.device;
            else if (recipient == 0 && dtype == 0x02) d = id_.config;
            else if (recipient == 0 && dtype == 0x03) d = id_.string_descriptor(dindex);
            else if (recipient == 1 && dtype == 0x22 && index < id_.report_descriptors.size())
                d = id_.report_descriptors[index];
            else if (recipient == 1 && dtype == 0x21) d = id_.hid_descriptor(uint8_t(index));
            if (d.empty()) return stall();
            return reply(d);
        }
        case kReqGetStatus: return reply({0, 0});
        case kReqGetConfiguration: return reply({1});
        case kReqGetInterface: return reply({0});
        case kReqSetAddress:
        case kReqSetConfiguration:
        case kReqSetInterface:
        case kReqClearFeature:
        case kReqSetFeature: return ack();
        default: return stall();
        }
    }

    if (type == 1 && recipient == 1) { // HID class, to an interface
        const bool gamepad = index == id_.gamepad_interface;
        const uint8_t rtype = to_uhid_rtype(uint8_t(value >> 8));
        const uint8_t rnum = uint8_t(value);
        switch (req) {
        case kHidSetIdle:
        case kHidSetProtocol: return ack();
        case kHidGetIdle: return reply({0});
        case kHidGetProtocol: return reply({1});
        case kHidGetReport: {
            if (!gamepad || !callbacks_.get_report) return stall();
            uint32_t rid;
            {
                std::lock_guard lock(mutex_);
                rid = next_request_id_++;
                control_[rid] = {seqnum, rnum, length};
            }
            callbacks_.get_report(rid, rtype, rnum);
            return;
        }
        case kHidSetReport: {
            if (!gamepad) return ack(); // e.g. keyboard LEDs on the lizard interface
            if (!callbacks_.set_report) return stall();
            std::vector<uint8_t> data;
            if (rnum == 0) data.push_back(0); // hidraw convention: leading report number
            data.insert(data.end(), out.begin(), out.end());
            uint32_t rid;
            {
                std::lock_guard lock(mutex_);
                rid = next_request_id_++;
                control_[rid] = {seqnum, rnum, uint16_t(out.size())};
            }
            callbacks_.set_report(rid, rtype, rnum, data.data(), data.size());
            return;
        }
        default: return stall();
        }
    }
    stall();
}

void UsbipDevice::reader_loop()
{
    std::vector<uint8_t> out;
    for (;;) {
        uint8_t h[kHeaderSize];
        if (!net::recv_all(sock_, h, sizeof(h))) break;
        const uint32_t command = get_be32(h + 0);
        const uint32_t seqnum = get_be32(h + 4);
        const uint32_t direction = get_be32(h + 12); // 0 out, 1 in
        const uint32_t ep = get_be32(h + 16);

        if (command == kCmdUnlink) {
            handle_unlink(seqnum, get_be32(h + 20));
            continue;
        }
        if (command != kCmdSubmit) {
            LOGE("usbip: unexpected command %u", command);
            break;
        }

        const uint32_t length = get_be32(h + 24);
        const int32_t packets = int32_t(get_be32(h + 32));
        const uint8_t* setup = h + 40;
        out.clear();
        if (direction == 0 && length > 0) {
            if (length > 65536) break;
            out.resize(length);
            if (!net::recv_all(sock_, out.data(), length)) break;
        }
        if (packets > 0 && packets != -1) { // isochronous descriptors: not used, skip
            std::vector<uint8_t> iso(size_t(packets) * 16);
            if (!net::recv_all(sock_, iso.data(), iso.size())) break;
        }

        if (ep == 0) {
            handle_control(seqnum, setup, out);
        } else if (direction == 1 && ep == id_.gamepad_in_ep) {
            std::lock_guard lock(mutex_);
            if (!reading_) {
                reading_ = true;
                LOGI("PC driver is now reading the controller");
                if (callbacks_.open_changed) callbacks_.open_changed(true);
            }
            if (!queued_input_.empty()) {
                const auto report = std::move(queued_input_.front());
                queued_input_.pop_front();
                complete_locked(seqnum, 0, true, report.data(),
                                std::min<size_t>(report.size(), length));
            } else {
                in_urbs_.push_back({seqnum, length});
            }
        } else if (direction == 0 && ep == id_.gamepad_out_ep) {
            std::vector<uint8_t> data{0};
            data.insert(data.end(), out.begin(), out.end());
            if (callbacks_.output) callbacks_.output(1, data.data(), data.size());
            complete(seqnum, 0, false, nullptr, out.size());
        } else if (direction == 1) {
            std::lock_guard lock(mutex_);
            parked_.insert(seqnum); // e.g. lizard-mode mouse/keyboard: never sends
        } else {
            complete(seqnum, 0, false, nullptr, out.size());
        }
    }
    LOGD("usbip: connection closed");
}

} // namespace steamu::host
