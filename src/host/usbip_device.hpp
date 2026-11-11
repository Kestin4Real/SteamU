// A virtual USB device attached through the kernel's USB/IP virtual host
// controller (vhci-hcd). Unlike uhid, this produces a real USB device in
// sysfs (interfaces, strings, bcdDevice), which Steam needs in order to
// recognize a Steam Deck controller: it only accepts the gamepad on USB
// interface 2.
//
// The kernel talks the USB/IP URB protocol over a local socket pair; this
// class answers it: descriptors and HID class requests locally, feature
// reports by forwarding them to the real Deck, and interrupt IN transfers
// with the latest input reports.
#pragma once

#include "host/usb_identity.hpp"
#include "host/virtual_controller.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace steamu::host {

class UsbipDevice final : public VirtualController {
public:
    UsbipDevice() = default;
    ~UsbipDevice() override;
    UsbipDevice(const UsbipDevice&) = delete;
    UsbipDevice& operator=(const UsbipDevice&) = delete;

    // Returns false (and logs why) if vhci-hcd is unavailable.
    bool create(UsbIdentity identity, ControllerCallbacks callbacks);
    void destroy();

    bool send_input(const uint8_t* data, size_t size) override;
    void reply_get_report(uint32_t id, int err, const uint8_t* data, size_t size) override;
    void reply_set_report(uint32_t id, int err) override;

private:
    struct InUrb {
        uint32_t seqnum;
        uint32_t length;
    };
    struct ControlRequest {
        uint32_t seqnum;
        uint8_t rnum;
        uint16_t length; // wLength (GET) or bytes received (SET)
    };

    void reader_loop();
    void handle_control(uint32_t seqnum, const uint8_t* setup, const std::vector<uint8_t>& out);
    void handle_unlink(uint32_t seqnum, uint32_t victim);

    // Sends RET_SUBMIT. Caller holds mutex_. data is sent only for IN.
    void complete_locked(uint32_t seqnum, int32_t status, bool in, const uint8_t* data,
                         size_t length);
    void complete(uint32_t seqnum, int32_t status, bool in, const uint8_t* data, size_t length);

    UsbIdentity id_;
    ControllerCallbacks callbacks_;
    int sock_ = -1;
    int port_ = -1;
    std::thread reader_;

    // Guards everything below *and* socket writes, so that a completion and
    // an unlink of the same URB can never be reordered.
    std::mutex mutex_;
    std::deque<InUrb> in_urbs_;                     // gamepad interrupt IN, waiting for data
    std::deque<std::vector<uint8_t>> queued_input_; // input that arrived with no URB pending
    std::set<uint32_t> parked_;                     // URBs on endpoints that never send
    std::map<uint32_t, ControlRequest> control_;    // forwarded to the deck, by request id
    uint32_t next_request_id_ = 1;
    bool reading_ = false;
};

} // namespace steamu::host
