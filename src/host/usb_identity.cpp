#include "host/usb_identity.hpp"

#include "common/log.hpp"

#include <cstring>

namespace steamu::host {

namespace {

constexpr uint8_t kDescDevice = 0x01;
constexpr uint8_t kDescConfig = 0x02;
constexpr uint8_t kDescInterface = 0x04;
constexpr uint8_t kDescEndpoint = 0x05;
constexpr uint8_t kDescHid = 0x21;

// Standard boot-protocol mouse and keyboard report descriptors, used for the
// emulated "lizard mode" interfaces 0 and 1 when synthesizing.
const uint8_t kBootMouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0,
};
const uint8_t kBootKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

void put_le16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}

UsbIdentity synthesize(const proto::HelloMsg& hello)
{
    UsbIdentity id;
    id.speed = 2;
    id.manufacturer = "Valve Software";
    id.product = "Steam Deck Controller";
    id.serial = "";
    id.gamepad_interface = 2;
    id.report_descriptors = {
        {std::begin(kBootMouse), std::end(kBootMouse)},
        {std::begin(kBootKeyboard), std::end(kBootKeyboard)},
        {hello.descriptor, hello.descriptor + hello.descriptor_size},
    };

    id.device = {18, kDescDevice, 0x00, 0x02, 0x00, 0x00, 0x00, 64};
    put_le16(id.device, hello.vendor);
    put_le16(id.device, hello.product);
    put_le16(id.device, uint16_t(hello.device_version ? hello.device_version : 0x0200));
    id.device.insert(id.device.end(), {1, 2, 0, 1}); // iManufacturer, iProduct, iSerial, configs

    std::vector<uint8_t>& c = id.config;
    c = {9, kDescConfig, 0, 0, 3, 1, 0, 0x80, 250}; // bus powered, 500 mA
    // interface subclass/protocol: boot mouse, boot keyboard, vendor gamepad
    const uint8_t sub[3] = {1, 1, 0}, proto_[3] = {2, 1, 0};
    const uint8_t max_packet[3] = {8, 8, 64}, interval[3] = {1, 1, 4};
    for (uint8_t i = 0; i < 3; ++i) {
        c.insert(c.end(), {9, kDescInterface, i, 0, 1, 0x03, sub[i], proto_[i], 0});
        c.insert(c.end(), {9, kDescHid, 0x11, 0x01, 0x00, 0x01, 0x22});
        put_le16(c, uint16_t(id.report_descriptors[i].size()));
        c.insert(c.end(), {7, kDescEndpoint, uint8_t(0x81 + i), 0x03, max_packet[i], 0, interval[i]});
    }
    c[2] = uint8_t(c.size());
    c[3] = uint8_t(c.size() >> 8);
    return id;
}

} // namespace

std::vector<uint8_t> UsbIdentity::hid_descriptor(uint8_t interface) const
{
    int current = -1;
    for (size_t i = 0; i + 2 <= config.size() && config[i] >= 2; i += config[i]) {
        if (config[i + 1] == kDescInterface && i + 2 < config.size()) current = config[i + 2];
        else if (config[i + 1] == kDescHid && current == interface && i + config[i] <= config.size())
            return {config.begin() + long(i), config.begin() + long(i + config[i])};
    }
    return {};
}

std::vector<uint8_t> UsbIdentity::string_descriptor(uint8_t index) const
{
    if (index == 0) return {4, 0x03, 0x09, 0x04}; // English (US)
    const std::string* s = nullptr;
    if (device.size() >= 18) {
        if (index == device[14]) s = &manufacturer;
        else if (index == device[15]) s = &product;
        else if (index == device[16]) s = &serial;
    }
    if (!s) return {};
    std::vector<uint8_t> d{0, 0x03};
    for (unsigned char ch : *s) { // ASCII is enough for these strings
        d.push_back(ch);
        d.push_back(0);
    }
    d[0] = uint8_t(std::min<size_t>(d.size(), 254));
    d.resize(d[0]);
    return d;
}

bool UsbIdentity::finalize()
{
    if (device.size() != 18 || device[1] != kDescDevice || config.size() < 9 ||
        config[1] != kDescConfig) {
        LOGE("invalid USB descriptors");
        return false;
    }
    const size_t total = size_t(config[2]) | size_t(config[3]) << 8;
    if (total > config.size()) {
        LOGE("configuration descriptor truncated");
        return false;
    }
    config.resize(total);

    int current = -1;
    for (size_t i = 0; i + 2 <= config.size() && config[i] >= 2; i += config[i]) {
        if (config[i + 1] == kDescInterface && i + 2 < config.size()) current = config[i + 2];
        else if (config[i + 1] == kDescEndpoint && current == gamepad_interface &&
                 i + 3 < config.size()) {
            const uint8_t addr = config[i + 2];
            const bool interrupt = (config[i + 3] & 0x03) == 0x03;
            if (!interrupt) continue;
            if (addr & 0x80) gamepad_in_ep = addr & 0x0F;
            else gamepad_out_ep = addr & 0x0F;
        }
    }
    if (!gamepad_in_ep) {
        LOGE("no interrupt IN endpoint on gamepad interface %u", gamepad_interface);
        return false;
    }
    return true;
}

UsbIdentity make_usb_identity(const proto::HelloMsg& hello)
{
    if (!hello.usb_valid || hello.usb_descriptors_size < 18 ||
        hello.usb_descriptors_size > proto::kMaxUsbDescriptors ||
        hello.interface_count > proto::kMaxInterfaces) {
        LOGI("deck sent no USB descriptors, using synthesized Steam Deck descriptors");
        return synthesize(hello);
    }

    UsbIdentity id;
    id.speed = hello.usb_speed;
    id.device.assign(hello.usb_descriptors, hello.usb_descriptors + 18);
    id.config.assign(hello.usb_descriptors + 18, hello.usb_descriptors + hello.usb_descriptors_size);
    id.manufacturer.assign(hello.usb_manufacturer, strnlen(hello.usb_manufacturer, sizeof(hello.usb_manufacturer)));
    id.product.assign(hello.usb_product, strnlen(hello.usb_product, sizeof(hello.usb_product)));
    id.serial.assign(hello.usb_serial, strnlen(hello.usb_serial, sizeof(hello.usb_serial)));
    id.gamepad_interface = hello.gamepad_interface;
    for (uint8_t i = 0; i < hello.interface_count; ++i) {
        const uint16_t n = std::min<uint16_t>(hello.interface_descriptor_size[i],
                                              uint16_t(proto::kMaxInterfaceDescriptor));
        id.report_descriptors.emplace_back(hello.interface_descriptor[i],
                                           hello.interface_descriptor[i] + n);
    }
    return id;
}

} // namespace steamu::host
