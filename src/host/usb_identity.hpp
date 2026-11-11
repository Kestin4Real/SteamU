// The USB-level identity of the emulated controller: descriptors, strings
// and the endpoints that carry gamepad data.
#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace steamu::host {

struct UsbIdentity {
    uint8_t speed = 2; // USB_SPEED_FULL
    std::vector<uint8_t> device; // 18-byte device descriptor
    std::vector<uint8_t> config; // full configuration descriptor (wTotalLength bytes)
    std::string manufacturer, product, serial;
    std::vector<std::vector<uint8_t>> report_descriptors; // HID report descriptor per interface
    uint8_t gamepad_interface = 0;

    // Derived from `config` by finalize().
    uint8_t gamepad_in_ep = 0;  // endpoint number (no direction bit), 0 = none
    uint8_t gamepad_out_ep = 0; // endpoint number, 0 = none

    // HID class descriptor (0x21) of an interface, copied out of `config`.
    std::vector<uint8_t> hid_descriptor(uint8_t interface) const;
    // String descriptor by index (0 = language list), empty if unknown.
    std::vector<uint8_t> string_descriptor(uint8_t index) const;

    bool finalize(); // validates descriptors and finds the gamepad endpoints
};

// Uses the Deck's real USB descriptors when it sent them, otherwise builds a
// Deck-like composite device (mouse, keyboard, gamepad on interface 2).
UsbIdentity make_usb_identity(const proto::HelloMsg& hello);

} // namespace steamu::host
