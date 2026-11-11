#include "deck/hid_backend.hpp"

#include "common/log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace steamu::deck {

namespace {

constexpr uint16_t kValveVendor = 0x28DE;
constexpr uint16_t kSteamDeckProduct = 0x1205;

std::string read_text_file(const std::string& path)
{
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

std::vector<uint8_t> read_binary_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

std::string real_path(const std::string& path)
{
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : std::string();
}

// Collects the USB-level identity of the device behind a hidraw node:
// raw device/config descriptors, strings, speed and every interface's HID
// report descriptor. Layout in sysfs:
//   /sys/class/hidraw/hidrawN/device -> .../<usbdev>/<usbdev>:1.<if>/0003:28DE:1205.XXXX
void read_usb_info(const std::string& sys_name, UsbInfo& usb)
{
    const std::string iface_dir = real_path("/sys/class/hidraw/" + sys_name + "/device/..");
    if (iface_dir.empty()) return;
    const std::string usb_dir = iface_dir.substr(0, iface_dir.find_last_of('/'));

    usb.descriptors = read_binary_file(usb_dir + "/descriptors");
    if (usb.descriptors.size() < 18 + 9) return; // not a USB device

    usb.manufacturer = read_text_file(usb_dir + "/manufacturer");
    usb.product = read_text_file(usb_dir + "/product");
    usb.serial = read_text_file(usb_dir + "/serial");
    const std::string speed = read_text_file(usb_dir + "/speed");
    usb.speed = speed == "1.5" ? 1 : speed == "12" ? 2 : 3;
    usb.gamepad_interface =
        uint8_t(std::strtoul(read_text_file(iface_dir + "/bInterfaceNumber").c_str(), nullptr, 16));

    DIR* dir = opendir(usb_dir.c_str());
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (!std::strchr(e->d_name, ':')) continue; // interfaces look like "3-3:1.2"
        const std::string ifd = usb_dir + "/" + e->d_name;
        const std::string num = read_text_file(ifd + "/bInterfaceNumber");
        if (num.empty()) continue;
        const size_t index = std::strtoul(num.c_str(), nullptr, 16);
        if (index >= 8) continue;
        if (usb.report_descriptors.size() <= index) usb.report_descriptors.resize(index + 1);

        DIR* sub = opendir(ifd.c_str());
        if (!sub) continue;
        while (dirent* h = readdir(sub)) {
            if (std::strncmp(h->d_name, "0003:", 5) != 0) continue; // HID device on bus USB
            usb.report_descriptors[index] = read_binary_file(ifd + "/" + h->d_name +
                                                             "/report_descriptor");
            break;
        }
        closedir(sub);
    }
    closedir(dir);
    usb.valid = usb.gamepad_interface < usb.report_descriptors.size();
    LOGI("USB identity: '%s' / '%s', %zu interfaces, gamepad on interface %u, speed %s",
         usb.manufacturer.c_str(), usb.product.c_str(), usb.report_descriptors.size(),
         usb.gamepad_interface, speed.c_str());
}

class HidrawBackend final : public HidBackend {
public:
    HidrawBackend(int fd, DeviceInfo info) : fd_(fd), info_(std::move(info)) {}
    ~HidrawBackend() override { ::close(fd_); }

    const DeviceInfo& info() const override { return info_; }

    int read_input(uint8_t* buf, size_t cap, int timeout_ms) override
    {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, timeout_ms);
        if (r < 0) return errno == EINTR ? 0 : -errno;
        if (r == 0) return 0;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        ssize_t n = ::read(fd_, buf, cap);
        if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -errno;
        return int(n);
    }

    int get_report(uint8_t rtype, uint8_t* buf, size_t cap) override
    {
        int r;
        switch (rtype) {
        case 0: r = ioctl(fd_, HIDIOCGFEATURE(cap), buf); break;
        case 2: r = ioctl(fd_, HIDIOCGINPUT(cap), buf); break;
        default: return -EINVAL;
        }
        return r < 0 ? -errno : r;
    }

    int set_report(uint8_t rtype, const uint8_t* data, size_t size) override
    {
        int r;
        switch (rtype) {
        case 0: r = ioctl(fd_, HIDIOCSFEATURE(size), data); break;
        case 1: r = ioctl(fd_, HIDIOCSOUTPUT(size), data); break;
        default: return -EINVAL;
        }
        return r < 0 ? -errno : 0;
    }

    int write_output(const uint8_t* data, size_t size) override
    {
        ssize_t n = ::write(fd_, data, size);
        return n < 0 ? -errno : 0;
    }

private:
    int fd_;
    DeviceInfo info_;
};

// Opens a hidraw node and fills in DeviceInfo. Returns -1 if it is not the
// Steam Deck controller's vendor (gamepad) interface.
int try_open(const std::string& dev_path, const std::string& sys_name, bool require_deck,
             DeviceInfo& info)
{
    int fd = ::open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (!require_deck) LOGE("cannot open %s: %s", dev_path.c_str(), strerror(errno));
        return -1;
    }

    hidraw_devinfo raw{};
    int desc_size = 0;
    if (ioctl(fd, HIDIOCGRAWINFO, &raw) < 0 || ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) < 0) {
        ::close(fd);
        return -1;
    }
    hidraw_report_descriptor desc{};
    desc.size = uint32_t(desc_size);
    if (ioctl(fd, HIDIOCGRDESC, &desc) < 0) {
        ::close(fd);
        return -1;
    }

    const bool is_deck = uint16_t(raw.vendor) == kValveVendor &&
                         uint16_t(raw.product) == kSteamDeckProduct;
    // The Deck exposes three interfaces: emulated keyboard, emulated mouse and
    // the real gamepad. The gamepad one uses a vendor usage page (06 xx FF).
    const bool is_vendor_page = desc.size >= 3 && desc.value[0] == 0x06 && desc.value[2] == 0xFF;
    if (require_deck && !(is_deck && is_vendor_page)) {
        ::close(fd);
        return -1;
    }

    char name[256] = {};
    ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name);
    char uniq[64] = {};
    ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq);

    info.name = name;
    info.uniq = uniq;
    info.bus = uint16_t(raw.bustype);
    info.vendor = uint16_t(raw.vendor);
    info.product = uint16_t(raw.product);
    info.descriptor.assign(desc.value, desc.value + desc.size);

    // bcdDevice lives on the USB device two levels above the HID device.
    std::string bcd = read_text_file("/sys/class/hidraw/" + sys_name + "/device/../../bcdDevice");
    info.version = bcd.empty() ? 0 : uint32_t(std::strtoul(bcd.c_str(), nullptr, 16));
    read_usb_info(sys_name, info.usb);
    return fd;
}

} // namespace

std::unique_ptr<HidBackend> open_hidraw_backend(const std::string& path_override)
{
    DeviceInfo info;
    if (!path_override.empty()) {
        std::string sys_name = path_override.substr(path_override.find_last_of('/') + 1);
        int fd = try_open(path_override, sys_name, false, info);
        if (fd < 0) return nullptr;
        LOGI("using %s (%s)", path_override.c_str(), info.name.c_str());
        return std::make_unique<HidrawBackend>(fd, std::move(info));
    }

    DIR* dir = opendir("/sys/class/hidraw");
    if (!dir) {
        LOGE("cannot list /sys/class/hidraw: %s", strerror(errno));
        return nullptr;
    }
    std::unique_ptr<HidBackend> result;
    while (dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "hidraw", 6) != 0) continue;
        std::string dev_path = std::string("/dev/") + entry->d_name;
        int fd = try_open(dev_path, entry->d_name, true, info);
        if (fd >= 0) {
            LOGI("found Steam Deck controller at %s (%s, descriptor %zu bytes)", dev_path.c_str(),
                 info.name.c_str(), info.descriptor.size());
            result = std::make_unique<HidrawBackend>(fd, std::move(info));
            break;
        }
    }
    closedir(dir);
    if (!result) LOGE("no Steam Deck controller (28DE:1205 vendor interface) found");
    return result;
}

} // namespace steamu::deck
