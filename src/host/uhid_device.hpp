// A virtual HID device created through /dev/uhid. The kernel and SDL accept
// it as a Steam Deck controller, but Steam does not: a uhid device has no
// USB parent, so it has no USB interface number and Steam only accepts the
// gamepad on interface 2. Kept as a fallback (steamu-host --uhid); the
// default is UsbipDevice.
#pragma once

#include "host/virtual_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace steamu::host {

class UhidDevice final : public VirtualController {
public:
    struct Info {
        std::string name;
        std::string phys;
        std::string uniq;
        uint16_t bus = 0;
        uint32_t vendor = 0;
        uint32_t product = 0;
        uint32_t version = 0;
        uint32_t country = 0;
        std::vector<uint8_t> descriptor;
    };

    using Callbacks = ControllerCallbacks;

    UhidDevice() = default;
    ~UhidDevice() override;
    UhidDevice(const UhidDevice&) = delete;
    UhidDevice& operator=(const UhidDevice&) = delete;

    bool create(const Info& info, Callbacks callbacks);
    void destroy();

    bool send_input(const uint8_t* data, size_t size) override;
    void reply_get_report(uint32_t id, int err, const uint8_t* data, size_t size) override;
    void reply_set_report(uint32_t id, int err) override;

private:
    void event_loop();
    bool write_event(const void* ev, size_t size);

    int fd_ = -1;
    int stop_fd_ = -1;
    Callbacks callbacks_;
    std::thread thread_;
};

} // namespace steamu::host
