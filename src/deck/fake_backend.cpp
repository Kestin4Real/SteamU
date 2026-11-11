// A synthetic Steam Deck controller so the whole PC side can be tested
// without the real hardware. Report layout follows SDL's
// SteamDeckStatePacket_t (src/joystick/hidapi/steam/controller_structs.h).
#include "deck/hid_backend.hpp"

#include "common/net.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace steamu::deck {

namespace {

// Minimal Valve-style vendor descriptor: 64-byte input, output and feature
// reports, no report IDs. The feature report is what hid-steam and SDL use
// to tell the gamepad interface apart from the emulated mouse/keyboard.
const uint8_t kFakeDescriptor[] = {
    0x06, 0x00, 0xFF, // Usage Page (Vendor 0xFF00)
    0x09, 0x01,       // Usage (0x01)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x40,       //   Report Count (64)
    0x09, 0x01,       //   Usage (0x01)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x95, 0x40,       //   Report Count (64)
    0x09, 0x01,       //   Usage (0x01)
    0x91, 0x02,       //   Output (Data,Var,Abs)
    0x95, 0x40,       //   Report Count (64)
    0x09, 0x01,       //   Usage (0x01)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0xC0,             // End Collection
};

constexpr uint8_t kIdControllerDeckState = 0x09;
constexpr uint8_t kIdGetStringAttribute = 0xAE;
constexpr uint8_t kAttribStrUnitSerial = 0x01;

// Low button word (ulButtonsL)
constexpr uint32_t kBtnA = 0x00000080;
constexpr uint32_t kBtnB = 0x00000020;
constexpr uint32_t kBtnX = 0x00000040;
constexpr uint32_t kBtnY = 0x00000010;
constexpr uint32_t kBtnL5 = 0x00008000;
constexpr uint32_t kBtnR5 = 0x00010000;
// High button word (ulButtonsH)
constexpr uint32_t kBtnL4 = 0x00000200;
constexpr uint32_t kBtnR4 = 0x00000400;

void put16(uint8_t* p, int16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

class FakeBackend final : public HidBackend {
public:
    explicit FakeBackend(bool idle) : idle_(idle)
    {
        info_.name = "Valve Software Steam Deck Controller (SteamU fake)";
        info_.uniq = "";
        info_.bus = 0x03; // BUS_USB
        info_.vendor = 0x28DE;
        info_.product = 0x1205;
        info_.version = 0x0200;
        info_.descriptor.assign(std::begin(kFakeDescriptor), std::end(kFakeDescriptor));
        start_us_ = next_us_ = net::now_us();
    }

    const DeviceInfo& info() const override { return info_; }

    int read_input(uint8_t* buf, size_t cap, int timeout_ms) override
    {
        if (cap < 64) return -EINVAL;
        uint64_t now = net::now_us();
        if (next_us_ > now + uint64_t(timeout_ms) * 1000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return 0;
        }
        if (next_us_ > now) std::this_thread::sleep_for(std::chrono::microseconds(next_us_ - now));
        next_us_ += 4000; // 250 Hz like the real Deck
        if (net::now_us() > next_us_ + 100000) next_us_ = net::now_us(); // don't burst after a stall

        const double t = double(net::now_us() - start_us_) / 1e6;
        std::memset(buf, 0, 64);
        buf[0] = 0x01; // unReportVersion (LE 0x0001)
        buf[1] = 0x00;
        buf[2] = kIdControllerDeckState;
        buf[3] = 64;
        put32(buf + 4, ++packet_num_);

        // Cycle through A, B, X, Y and the four back buttons, 0.5 s each.
        static constexpr struct { uint32_t lo, hi; } kCycle[] = {
            {kBtnA, 0}, {kBtnB, 0}, {kBtnX, 0}, {kBtnY, 0},
            {kBtnL5, 0}, {kBtnR5, 0}, {0, kBtnL4}, {0, kBtnR4},
        };
        if (idle_) {
            // Nothing moves; only buttons scripted via press_buttons().
            put32(buf + 8, scripted_lo_.load());
            put32(buf + 12, scripted_hi_.load());
            return 64;
        }
        const auto& b = kCycle[size_t(t * 2.0) % std::size(kCycle)];
        put32(buf + 8, b.lo);
        put32(buf + 12, b.hi);

        const auto s = [](double v) { return int16_t(std::lround(v * 32767.0)); };
        put16(buf + 24, 0);                        // accel X
        put16(buf + 26, 16384);                    // accel Y (~1 g)
        put16(buf + 28, 0);                        // accel Z
        put16(buf + 30, s(0.2 * std::sin(t * 3))); // gyro X
        put16(buf + 32, s(0.2 * std::cos(t * 3))); // gyro Y
        put16(buf + 34, 0);                        // gyro Z
        put16(buf + 48, s(std::cos(t)));           // left stick X
        put16(buf + 50, s(std::sin(t)));           // left stick Y
        put16(buf + 52, s(std::cos(-t)));          // right stick X
        put16(buf + 54, s(std::sin(-t)));          // right stick Y
        return 64;
    }

    int get_report(uint8_t rtype, uint8_t* buf, size_t cap) override
    {
        if (rtype != 0 || cap < 65) return -EINVAL;
        std::lock_guard lock(mutex_);
        std::memset(buf, 0, 65);
        // buf[0] stays 0: report number. Answer the last command sent.
        buf[1] = last_cmd_[0];
        if (last_cmd_[0] == kIdGetStringAttribute) {
            const char serial[] = "FAKEDECK01";
            buf[2] = uint8_t(sizeof(serial)); // payload length incl. attribute byte
            buf[3] = kAttribStrUnitSerial;
            std::memcpy(buf + 4, serial, sizeof(serial) - 1);
        }
        return 65;
    }

    int set_report(uint8_t rtype, const uint8_t* data, size_t size) override
    {
        if (rtype != 0 || size < 2) return -EINVAL;
        std::lock_guard lock(mutex_);
        std::memset(last_cmd_, 0, sizeof(last_cmd_));
        std::memcpy(last_cmd_, data + 1, std::min(size - 1, sizeof(last_cmd_)));
        return 0;
    }

    int write_output(const uint8_t*, size_t) override { return 0; }

private:
    DeviceInfo info_;
    bool idle_ = false;

public:
    void press(uint32_t lo, uint32_t hi, bool down)
    {
        if (down) {
            scripted_lo_ |= lo;
            scripted_hi_ |= hi;
        } else {
            scripted_lo_ &= ~lo;
            scripted_hi_ &= ~hi;
        }
    }

private:
    std::atomic<uint32_t> scripted_lo_{0}, scripted_hi_{0};
    uint64_t start_us_ = 0;
    uint64_t next_us_ = 0;
    uint32_t packet_num_ = 0;
    std::mutex mutex_;
    uint8_t last_cmd_[64] = {};
};

} // namespace

std::unique_ptr<HidBackend> open_fake_backend(bool idle)
{
    return std::make_unique<FakeBackend>(idle);
}

bool fake_press(HidBackend& backend, const std::string& name, bool down)
{
    // SUS_DECK_* bits (sdk/steamu_screen.h); hi-word buttons flagged.
    static const struct { const char* name; uint32_t bit; bool hi; } kButtons[] = {
        {"R2", 0x1, false}, {"L2", 0x2, false}, {"R1", 0x4, false}, {"L1", 0x8, false},
        {"Y", 0x10, false}, {"B", 0x20, false}, {"X", 0x40, false}, {"A", 0x80, false},
        {"DPAD_UP", 0x100, false}, {"DPAD_RIGHT", 0x200, false}, {"DPAD_LEFT", 0x400, false},
        {"DPAD_DOWN", 0x800, false}, {"VIEW", 0x1000, false}, {"MENU", 0x4000, false},
        {"L5", 0x8000, false}, {"R5", 0x10000, false}, {"RPAD_TOUCH", 0x100000, false},
        {"L3", 0x400000, false}, {"R3", 0x4000000, false}, {"L4", 0x200, true}, {"R4", 0x400, true},
    };
    auto* fake = dynamic_cast<FakeBackend*>(&backend);
    if (!fake) return false;
    for (const auto& b : kButtons) {
        if (name == b.name) {
            fake->press(b.hi ? 0 : b.bit, b.hi ? b.bit : 0, down);
            return true;
        }
    }
    return false;
}

} // namespace steamu::deck
