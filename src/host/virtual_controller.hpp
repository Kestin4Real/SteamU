// Common interface of the PC-side controller emulations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace steamu::host {

// Requests coming from the PC side (Steam, SDL, kernel driver) that must be
// executed on the real controller. Report data uses the hidraw convention:
// byte 0 is the report number. rtype uses uhid values (0 feature, 1 output,
// 2 input). GET/SET requests must be answered with reply_*_report().
struct ControllerCallbacks {
    std::function<void(uint32_t id, uint8_t rtype, uint8_t rnum)> get_report;
    std::function<void(uint32_t id, uint8_t rtype, uint8_t rnum, const uint8_t* data, size_t size)>
        set_report;
    std::function<void(uint8_t rtype, const uint8_t* data, size_t size)> output;
    std::function<void(bool opened)> open_changed;
};

class VirtualController {
public:
    virtual ~VirtualController() = default;

    // Raw input report as read from hidraw on the Deck.
    virtual bool send_input(const uint8_t* data, size_t size) = 0;

    // data in hidraw convention (byte 0 = report number). err = positive errno.
    virtual void reply_get_report(uint32_t id, int err, const uint8_t* data, size_t size) = 0;
    virtual void reply_set_report(uint32_t id, int err) = 0;
};

} // namespace steamu::host
