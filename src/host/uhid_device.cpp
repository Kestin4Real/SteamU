#include "host/uhid_device.hpp"

#include "common/log.hpp"

#include <fcntl.h>
#include <linux/uhid.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>

namespace steamu::host {

namespace {

// Size of an event up to and including the first `payload` bytes of the union.
constexpr size_t event_size(size_t payload)
{
    return offsetof(uhid_event, u) + payload;
}

} // namespace

UhidDevice::~UhidDevice()
{
    destroy();
}

bool UhidDevice::create(const Info& info, Callbacks callbacks)
{
    destroy();
    if (info.descriptor.empty() || info.descriptor.size() > HID_MAX_DESCRIPTOR_SIZE) {
        LOGE("invalid HID descriptor size %zu", info.descriptor.size());
        return false;
    }

    fd_ = ::open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        LOGE("cannot open /dev/uhid: %s%s", strerror(errno),
             errno == EACCES ? " (install packaging/60-steamu.rules, see README)" : "");
        return false;
    }

    auto ev = std::make_unique<uhid_event>();
    std::memset(ev.get(), 0, sizeof(*ev));
    ev->type = UHID_CREATE2;
    auto& c = ev->u.create2;
    std::snprintf(reinterpret_cast<char*>(c.name), sizeof(c.name), "%s", info.name.c_str());
    std::snprintf(reinterpret_cast<char*>(c.phys), sizeof(c.phys), "%s", info.phys.c_str());
    std::snprintf(reinterpret_cast<char*>(c.uniq), sizeof(c.uniq), "%s", info.uniq.c_str());
    c.rd_size = uint16_t(info.descriptor.size());
    c.bus = info.bus;
    c.vendor = info.vendor;
    c.product = info.product;
    c.version = info.version;
    c.country = info.country;
    std::memcpy(c.rd_data, info.descriptor.data(), info.descriptor.size());

    if (!write_event(ev.get(), sizeof(*ev))) {
        LOGE("UHID_CREATE2 failed: %s", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    callbacks_ = std::move(callbacks);
    stop_fd_ = eventfd(0, EFD_CLOEXEC);
    thread_ = std::thread([this] { event_loop(); });
    LOGI("virtual controller created: '%s' %04X:%04X", info.name.c_str(), info.vendor,
         info.product);
    return true;
}

void UhidDevice::destroy()
{
    if (fd_ < 0) return;
    uint64_t one = 1;
    if (write(stop_fd_, &one, sizeof(one)) < 0) { /* thread also exits on fd errors */ }
    if (thread_.joinable()) thread_.join();

    uhid_event ev{};
    ev.type = UHID_DESTROY;
    write_event(&ev, event_size(0));
    ::close(fd_);
    ::close(stop_fd_);
    fd_ = stop_fd_ = -1;
    LOGI("virtual controller removed");
}

bool UhidDevice::write_event(const void* ev, size_t size)
{
    for (;;) {
        ssize_t n = ::write(fd_, ev, size);
        if (n >= 0) return true;
        if (errno != EINTR) return false;
    }
}

bool UhidDevice::send_input(const uint8_t* data, size_t size)
{
    if (fd_ < 0 || size > UHID_DATA_MAX) return false;
    uhid_event ev;
    ev.type = UHID_INPUT2;
    ev.u.input2.size = uint16_t(size);
    std::memcpy(ev.u.input2.data, data, size);
    return write_event(&ev, event_size(offsetof(uhid_input2_req, data) + size));
}

void UhidDevice::reply_get_report(uint32_t id, int err, const uint8_t* data, size_t size)
{
    if (fd_ < 0) return;
    size = std::min<size_t>(size, UHID_DATA_MAX);
    uhid_event ev;
    ev.type = UHID_GET_REPORT_REPLY;
    ev.u.get_report_reply.id = id;
    ev.u.get_report_reply.err = uint16_t(err);
    ev.u.get_report_reply.size = err ? 0 : uint16_t(size);
    if (!err && size) std::memcpy(ev.u.get_report_reply.data, data, size);
    write_event(&ev, event_size(offsetof(uhid_get_report_reply_req, data) + (err ? 0 : size)));
}

void UhidDevice::reply_set_report(uint32_t id, int err)
{
    if (fd_ < 0) return;
    uhid_event ev{};
    ev.type = UHID_SET_REPORT_REPLY;
    ev.u.set_report_reply.id = id;
    ev.u.set_report_reply.err = uint16_t(err);
    write_event(&ev, event_size(sizeof(uhid_set_report_reply_req)));
}

void UhidDevice::event_loop()
{
    auto ev = std::make_unique<uhid_event>();
    for (;;) {
        pollfd fds[2] = {{fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            LOGE("uhid poll failed: %s", strerror(errno));
            return;
        }
        if (fds[1].revents) return;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return;
        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t n = ::read(fd_, ev.get(), sizeof(*ev));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOGE("uhid read failed: %s", strerror(errno));
            return;
        }

        switch (ev->type) {
        case UHID_START: LOGD("uhid: start"); break;
        case UHID_STOP: LOGD("uhid: stop"); break;
        case UHID_OPEN:
            LOGI("controller opened by the PC (Steam / a game is reading it)");
            if (callbacks_.open_changed) callbacks_.open_changed(true);
            break;
        case UHID_CLOSE:
            LOGI("controller closed by the PC");
            if (callbacks_.open_changed) callbacks_.open_changed(false);
            break;
        case UHID_OUTPUT: {
            const auto& o = ev->u.output;
            if (callbacks_.output) callbacks_.output(o.rtype, o.data, o.size);
            break;
        }
        case UHID_GET_REPORT: {
            const auto& g = ev->u.get_report;
            if (callbacks_.get_report) callbacks_.get_report(g.id, g.rtype, g.rnum);
            else reply_get_report(g.id, EIO, nullptr, 0);
            break;
        }
        case UHID_SET_REPORT: {
            const auto& s = ev->u.set_report;
            if (callbacks_.set_report)
                callbacks_.set_report(s.id, s.rtype, s.rnum, s.data,
                                      std::min<size_t>(s.size, UHID_DATA_MAX));
            else reply_set_report(s.id, EIO);
            break;
        }
        default: LOGD("uhid: event %u", ev->type); break;
        }
    }
}

} // namespace steamu::host
