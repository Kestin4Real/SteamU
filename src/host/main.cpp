// steamu-host: runs on the PC. Accepts one Steam Deck at a time, exposes its
// controller as a local virtual Steam Deck controller (USB/IP) and streams
// the screen to it.
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"
#include "host/audio_streamer.hpp"
#include "host/uhid_device.hpp"
#include "host/usb_identity.hpp"
#include "host/screen_server.hpp"
#include "host/usbip_device.hpp"
#include "host/video_streamer.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

using namespace steamu;

namespace {

struct Session {
    std::unique_ptr<net::Channel> channel;
    std::unique_ptr<host::VirtualController> device;
    host::VideoStreamer video;
    host::AudioStreamer audio;
    uint32_t token = 0;

    // Owned by the UDP input thread.
    bool have_seq = false;
    uint32_t last_seq = 0;
    bool neutral_sent = false; // virtual controller parked while a game owns the controls

    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> lost{0};
    std::atomic<uint64_t> late{0};
    std::atomic<uint64_t> rtt_us{0};
};

std::mutex g_session_mutex;
std::shared_ptr<Session> g_session;
bool g_use_uhid = false;
bool g_video_enabled = true;
host::VideoOptions g_video_options;
bool g_audio_enabled = true;
host::AudioOptions g_audio_options;
host::ScreenServer g_screens;
// --port N: control N, input N+1, discovery N+2 (lets a test instance run
// next to the real one).
uint16_t g_control_port = proto::kControlPort;
uint16_t g_input_port = proto::kInputPort;
uint16_t g_discovery_port = proto::kDiscoveryPort;

std::shared_ptr<Session> current_session()
{
    std::lock_guard lock(g_session_mutex);
    return g_session;
}

int bind_socket(int type, uint16_t port)
{
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("cannot bind port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// Answers LAN broadcasts from steamu-deck so no IP has to be typed in.
void discovery_thread(int fd)
{
    proto::DiscoveryReply reply{};
    reply.magic = proto::kMagic;
    reply.version = proto::kVersion;
    reply.control_port = g_control_port;
    gethostname(reply.hostname, sizeof(reply.hostname) - 1);

    for (;;) {
        proto::DiscoveryRequest req{};
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(fd, &req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n != sizeof(req) || req.magic != proto::kMagic) continue;
        LOGD("discovery request from %s", net::to_string(from).c_str());
        sendto(fd, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), len);
    }
}

// Hot path: UDP input report -> virtual controller.
void input_thread(int fd)
{
    net::tune_udp(fd);
    alignas(8) uint8_t buf[2048];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < ssize_t(sizeof(proto::InputPacketHeader))) continue;
        proto::InputPacketHeader hdr;
        std::memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != proto::kMagic || hdr.size > size_t(n) - sizeof(hdr)) continue;

        std::shared_ptr<Session> s = current_session();
        if (!s || hdr.token != s->token) continue;

        if (s->have_seq) {
            int32_t delta = int32_t(hdr.seq - s->last_seq);
            if (delta <= 0) { // older than what we already applied
                s->late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (delta > 1) s->lost.fetch_add(uint64_t(delta - 1), std::memory_order_relaxed);
        }
        s->have_seq = true;
        s->last_seq = hdr.seq;
        const uint8_t* report = buf + sizeof(hdr);
        if (g_screens.controller_report(report, hdr.size)) {
            // A second-screen game drives the controls itself: hold the PC's
            // virtual controller in a neutral state so Steam can't also act.
            if (!s->neutral_sent && hdr.size >= 16 && hdr.size <= 512) {
                uint8_t neutral[512];
                std::memcpy(neutral, report, hdr.size);
                std::memset(neutral + 8, 0, hdr.size - 8); // buttons, pads, IMU, sticks
                s->device->send_input(neutral, hdr.size);
                s->neutral_sent = true;
            }
        } else {
            s->neutral_sent = false;
            s->device->send_input(report, hdr.size);
        }
        s->packets.fetch_add(1, std::memory_order_relaxed);
    }
}

host::UhidDevice::Info device_info_from(const proto::HelloMsg& hello, const sockaddr_in& peer)
{
    host::UhidDevice::Info info;
    info.name.assign(hello.name, strnlen(hello.name, sizeof(hello.name)));
    info.uniq.assign(hello.uniq, strnlen(hello.uniq, sizeof(hello.uniq)));
    info.phys = "steamu/" + net::to_string(peer);
    info.bus = hello.bus;
    info.vendor = hello.vendor;
    info.product = hello.product;
    info.version = hello.device_version;
    info.country = hello.country;
    info.descriptor.assign(hello.descriptor, hello.descriptor + hello.descriptor_size);
    return info;
}

void run_session(int client_fd, const sockaddr_in& peer, std::mt19937& rng)
{
    auto s = std::make_shared<Session>();
    s->channel = std::make_unique<net::Channel>(client_fd);
    net::Channel& ch = *s->channel;

    // --- Handshake -------------------------------------------------------
    proto::Msg type{};
    std::vector<uint8_t> payload;
    if (!ch.recv(type, payload) || type != proto::Msg::Hello ||
        payload.size() < sizeof(proto::HelloMsg)) {
        LOGW("%s: bad handshake", net::to_string(peer).c_str());
        return;
    }
    auto hello = std::make_unique<proto::HelloMsg>();
    std::memcpy(hello.get(), payload.data(), sizeof(*hello));
    if (hello->magic != proto::kMagic || hello->version != proto::kVersion) {
        LOGW("%s: protocol mismatch (v%u, we are v%u)", net::to_string(peer).c_str(),
             hello->version, proto::kVersion);
        return;
    }
    if (hello->descriptor_size == 0 || hello->descriptor_size > proto::kMaxDescriptor) {
        LOGW("%s: invalid descriptor size", net::to_string(peer).c_str());
        return;
    }
    hello->hostname[sizeof(hello->hostname) - 1] = 0;
    LOGI("deck '%s' connected from %s", hello->hostname, net::to_string(peer).c_str());

    // --- Virtual controller ----------------------------------------------
    // Requests from the PC (Steam, SDL, hid-steam) are forwarded to the real
    // controller; the deck's answer is fed back in the receive loop below.
    Session* sp = s.get();
    host::ControllerCallbacks cb;
    cb.get_report = [sp](uint32_t id, uint8_t rtype, uint8_t rnum) {
        proto::GetReportMsg m{id, rtype, rnum};
        if (!sp->channel->send(proto::Msg::GetReport, &m, sizeof(m)))
            sp->device->reply_get_report(id, EIO, nullptr, 0);
    };
    cb.set_report = [sp](uint32_t id, uint8_t rtype, uint8_t rnum, const uint8_t* data,
                         size_t size) {
        proto::SetReportMsg m{id, rtype, rnum};
        LOGD("set_report rtype=%u cmd=0x%02x size=%zu", rtype, size > 1 ? data[1] : 0, size);
        if (!sp->channel->send(proto::Msg::SetReport, &m, sizeof(m), data, size))
            sp->device->reply_set_report(id, EIO);
    };
    cb.output = [sp](uint8_t rtype, const uint8_t* data, size_t size) {
        proto::OutputReportMsg m{rtype};
        sp->channel->send(proto::Msg::OutputReport, &m, sizeof(m), data, size);
    };

    // Welcome goes out before the device exists: creating it makes the kernel
    // driver send GetReport requests at once, and the deck must already be
    // past the handshake to answer them.
    s->token = uint32_t(rng());
    proto::WelcomeMsg welcome{proto::kMagic, proto::kVersion, g_input_port, s->token};
    if (!ch.send(proto::Msg::Welcome, &welcome, sizeof(welcome))) return;
    if (g_use_uhid) {
        auto dev = std::make_unique<host::UhidDevice>();
        if (!dev->create(device_info_from(*hello, peer), cb)) return;
        s->device = std::move(dev);
    } else {
        auto dev = std::make_unique<host::UsbipDevice>();
        if (!dev->create(host::make_usb_identity(*hello), cb)) return;
        s->device = std::move(dev);
    }
    // Publish only after create(): the input thread may now use the device.
    {
        std::lock_guard lock(g_session_mutex);
        g_session = s;
    }

    // --- Ping / stats ----------------------------------------------------
    std::mutex stop_mutex;
    std::condition_variable stop_cv;
    bool stop = false;
    std::thread stats([&] {
        uint64_t last_packets = 0, last_lost = 0, last_late = 0;
        int tick = 0;
        std::unique_lock lock(stop_mutex);
        while (!stop_cv.wait_for(lock, std::chrono::seconds(1), [&] { return stop; })) {
            proto::PingMsg ping{net::now_us(), s->rtt_us.load()};
            ch.send(proto::Msg::Ping, &ping, sizeof(ping));
            if (++tick % 5 != 0) continue;
            uint64_t p = s->packets, l = s->lost, la = s->late;
            LOGI("input %.0f reports/s, lost %llu, late %llu, rtt %.1f ms",
                 double(p - last_packets) / 5.0, (unsigned long long)(l - last_lost),
                 (unsigned long long)(la - last_late), double(s->rtt_us) / 1000.0);
            last_packets = p;
            last_lost = l;
            last_late = la;
        }
    });

    // --- Replies from the deck -------------------------------------------
    while (ch.recv(type, payload)) {
        switch (type) {
        case proto::Msg::GetReportReply: {
            if (payload.size() < sizeof(proto::ReportReplyMsg)) break;
            proto::ReportReplyMsg r{};
            std::memcpy(&r, payload.data(), sizeof(r));
            s->device->reply_get_report(r.id, r.err, payload.data() + sizeof(r),
                                       payload.size() - sizeof(r));
            break;
        }
        case proto::Msg::SetReportReply: {
            if (payload.size() < sizeof(proto::ReportReplyMsg)) break;
            proto::ReportReplyMsg r{};
            std::memcpy(&r, payload.data(), sizeof(r));
            s->device->reply_set_report(r.id, r.err);
            break;
        }
        case proto::Msg::Pong: {
            if (payload.size() < sizeof(proto::PingMsg)) break;
            proto::PingMsg p{};
            std::memcpy(&p, payload.data(), sizeof(p));
            s->rtt_us = net::now_us() - p.host_time_us;
            break;
        }
        case proto::Msg::VideoStart: {
            if (payload.size() < sizeof(proto::VideoStartMsg)) break;
            proto::VideoStartMsg v{};
            std::memcpy(&v, payload.data(), sizeof(v));
            if (!g_video_enabled) {
                LOGI("deck asked for video, but --no-video is set");
                break;
            }
            if (v.width < 64 || v.height < 64 || v.width > 4096 || v.height > 4096) break;
            LOGI("deck display %ux%u @ %u Hz, video port %u", v.width, v.height, v.refresh_hz,
                 v.udp_port);
            sockaddr_in video_addr = peer;
            video_addr.sin_port = htons(v.udp_port);
            g_screens.set_display(true, v.width, v.height, v.refresh_hz);
            s->video.start(g_video_options, video_addr, v.width, v.height, v.refresh_hz,
                           &g_screens);
            break;
        }
        case proto::Msg::AudioStart: {
            if (payload.size() < sizeof(proto::AudioStartMsg)) break;
            proto::AudioStartMsg a{};
            std::memcpy(&a, payload.data(), sizeof(a));
            if (!g_audio_enabled) {
                LOGI("deck can play audio, but --no-audio is set");
                break;
            }
            sockaddr_in audio_addr = peer;
            audio_addr.sin_port = htons(a.udp_port);
            s->audio.start(g_audio_options, audio_addr, &g_screens);
            break;
        }
        case proto::Msg::Touch: {
            if (payload.size() < sizeof(proto::TouchMsg)) break;
            proto::TouchMsg t{};
            std::memcpy(&t, payload.data(), sizeof(t));
            g_screens.send_touch(t.type, t.finger, t.x, t.y);
            break;
        }
        case proto::Msg::RequestKeyframe:
            LOGD("deck requested a keyframe");
            s->video.request_keyframe();
            break;
        default: LOGW("unexpected message type %u", unsigned(type)); break;
        }
    }

    s->video.stop();
    s->audio.stop();
    g_screens.set_display(false, 0, 0, 0);
    LOGI("deck '%s' disconnected", hello->hostname);
    {
        std::lock_guard lock(g_session_mutex);
        g_session.reset();
    }
    {
        std::lock_guard lock(stop_mutex);
        stop = true;
    }
    stop_cv.notify_all();
    stats.join();
    // The device is destroyed with the Session, once the input thread has
    // dropped any reference it still holds (at most one packet later).
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v" || a == "--verbose") g_verbose = true;
        else if (a == "--uhid") g_use_uhid = true;
        else if (a == "--no-video") g_video_enabled = false;
        else if (a == "--no-audio") g_audio_enabled = false;
        else if (a == "--no-desktop-audio") g_audio_options.desktop = false;
        else if (a == "--audio-source" && i + 1 < argc) g_audio_options.source = argv[++i];
        else if (a == "--audio-bitrate" && i + 1 < argc) g_audio_options.bitrate_kbps = std::atoi(argv[++i]);
        else if (a == "--audio-frame" && i + 1 < argc) g_audio_options.frame_ms = std::atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) {
            g_control_port = uint16_t(std::atoi(argv[++i]));
            g_input_port = uint16_t(g_control_port + 1);
            g_discovery_port = uint16_t(g_control_port + 2);
        }
        else if (a == "--monitor" && i + 1 < argc) g_video_options.monitor = argv[++i];
        else if (a == "--codec" && i + 1 < argc) g_video_options.codec = argv[++i];
        else if (a == "--bitrate" && i + 1 < argc) g_video_options.bitrate_kbps = std::atoi(argv[++i]) * 1000;
        else if (a == "--fps" && i + 1 < argc) g_video_options.fps = std::atoi(argv[++i]);
        else if (a == "--obs") g_video_options.obs_url = "udp://127.0.0.1:27860?pkt_size=1316";
        else if (a == "--obs-url" && i + 1 < argc) g_video_options.obs_url = argv[++i];
        else {
            std::fprintf(stderr, "usage: steamu-host [options]\n"
                         "  --monitor NAME  monitor to mirror, e.g. DP-2 (default: primary)\n"
                         "  --codec C       hevc (default) or h264\n"
                         "  --bitrate MBPS  video bitrate in Mbit/s (default 20)\n"
                         "  --fps N         frame rate (default: the Deck's refresh rate)\n"
                         "  --obs           also send the Deck's picture to OBS:\n"
                         "                  Media Source, URL udp://127.0.0.1:27860\n"
                         "  --obs-url URL   same, to another address (any FFmpeg output URL)\n"
                         "  --no-video      no video\n"
                         "  --no-audio      no audio at all\n"
                         "  --no-desktop-audio  only second-screen apps' own Deck audio\n"
                         "  --audio-source S    PulseAudio source to send (default: monitor\n"
                         "                  of the default output, i.e. what the PC plays)\n"
                         "  --audio-bitrate K   Opus bitrate in kbit/s (default 128)\n"
                         "  --audio-frame MS    Opus frame size: 5 (default), 10 or 20\n"
                         "  --port N        use ports N..N+2 instead of 27850..27852\n"
                         "  -v              verbose logging\n"
                         "  --uhid          use uhid instead of USB/IP (Steam won't detect it)\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    int listen_fd = bind_socket(SOCK_STREAM, g_control_port);
    int input_fd = bind_socket(SOCK_DGRAM, g_input_port);
    int discovery_fd = bind_socket(SOCK_DGRAM, g_discovery_port);
    if (listen_fd < 0 || input_fd < 0 || discovery_fd < 0) return 1;
    listen(listen_fd, 1);

    g_screens.start();
    std::thread(input_thread, input_fd).detach();
    std::thread(discovery_thread, discovery_fd).detach();
    LOGI("steamu-host listening (control tcp/%u, input udp/%u, discovery udp/%u)",
         g_control_port, g_input_port, g_discovery_port);

    std::mt19937 rng{std::random_device{}()};
    for (;;) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EINTR) LOGE("accept failed: %s", strerror(errno));
            continue;
        }
        net::tune_tcp(fd);
        run_session(fd, peer, rng); // one deck at a time
    }
}
