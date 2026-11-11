// Source of controller data on the Deck side: either the real built-in
// controller (hidraw) or a synthetic one for testing without a Deck.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace steamu::deck {

struct UsbInfo {
    bool valid = false;
    uint8_t speed = 2; // USB_SPEED_FULL
    uint8_t gamepad_interface = 0;
    std::vector<uint8_t> descriptors; // device + config descriptors (sysfs "descriptors")
    std::string manufacturer, product, serial;
    std::vector<std::vector<uint8_t>> report_descriptors; // indexed by interface number
};

struct DeviceInfo {
    UsbInfo usb;
    std::string name;
    std::string uniq;
    uint16_t bus = 0;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint32_t version = 0;
    std::vector<uint8_t> descriptor;
};

class HidBackend {
public:
    virtual ~HidBackend() = default;

    virtual const DeviceInfo& info() const = 0;

    // Blocks up to timeout_ms. Returns report size, 0 on timeout, <0 on error.
    virtual int read_input(uint8_t* buf, size_t cap, int timeout_ms) = 0;

    // buf[0] = report number on entry; on success buf holds the report
    // (byte 0 = report number) and the size is returned. <0 = -errno.
    virtual int get_report(uint8_t rtype, uint8_t* buf, size_t cap) = 0;

    // data[0] = report number. Returns 0 or -errno.
    virtual int set_report(uint8_t rtype, const uint8_t* data, size_t size) = 0;

    // data[0] = report number. Returns 0 or -errno.
    virtual int write_output(const uint8_t* data, size_t size) = 0;
};

// Finds the Steam Deck controller's vendor interface (28DE:1205) unless an
// explicit /dev/hidrawN path is given.
std::unique_ptr<HidBackend> open_hidraw_backend(const std::string& path_override);

// Synthetic Steam Deck: sticks move in circles, buttons cycle, gyro rotates.
// idle = a connected controller nobody touches (all neutral).
std::unique_ptr<HidBackend> open_fake_backend(bool idle = false);

// Presses/releases a button on an idle fake backend by name (A, B, X, Y,
// L1..R5, DPAD_UP, VIEW, MENU, ...). For scripted tests.
bool fake_press(HidBackend& backend, const std::string& name, bool down);

} // namespace steamu::deck
