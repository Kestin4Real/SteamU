// steamu-deck: runs on the Steam Deck (best from Game Mode).
//  * Streams the built-in controller's raw HID reports to the PC and executes
//    the PC's feature/output reports on the real controller.
//  * Shows the PC's screen fullscreen (SDL3), decoded with VAAPI.
// Networking runs on a background thread; the main thread only presents.
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"
#include "deck/hid_backend.hpp"
#include "deck/audio_receiver.hpp"
#include "deck/video_receiver.hpp"

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace steamu;

namespace {

struct Options {
    std::string host;
    std::string hidraw;
    bool fake = false;
    bool fake_idle = false;
    bool fake_stdin = false;
    bool windowed = false;
    bool vsync = false;
    bool video = true;
    bool audio = true;
    int audio_buffer_ms = 30;
    uint16_t port = proto::kControlPort;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: steamu-deck [options]\n"
                 "  --host IP     PC address (default: find it with a LAN broadcast)\n"
                 "  --hidraw DEV  controller device (default: auto-detect 28DE:1205)\n"
                 "  --fake        send a synthetic controller (testing without a Deck)\n"
                 "  --fake-idle   like --fake, but nothing is pressed\n"
                 "  --fake-stdin  idle fake whose buttons are scripted on stdin:\n"
                 "                \"tap A\", \"hold R2\", \"release R2\" (one per line)\n"
                 "  --windowed    1280x800 window instead of fullscreen\n"
                 "  --vsync       wait for vsync when presenting (no tearing, +latency)\n"
                 "  --no-video    controller only\n"
                 "  --no-audio    don't play the PC's audio\n"
                 "  --audio-buffer MS  audio jitter buffer (default 30; lower = less\n"
                 "                delay, but more dropouts on a busy Wi-Fi)\n"
                 "  --port N      host control port (with --host)\n"
                 "  -v            verbose logging\n");
}

// State shared between the network thread and the display (main) thread.
struct App {
    Options opt;
    int display_w = 1280, display_h = 800, refresh_hz = 60;
    Uint32 frame_event = 0;
    std::atomic<bool> quit{false};

    std::mutex mutex;
    std::string status = "Starting...";
    AVFrame* latest = nullptr; // newest decoded frame not yet shown
    bool connected = false;
    net::Channel* channel = nullptr; // valid while connected (guarded by mutex)

    // deck clock - host clock, from the ping with the lowest round trip.
    std::atomic<int64_t> clock_offset_us{0};
    std::atomic<bool> clock_valid{false};
    uint64_t best_rtt_us = UINT64_MAX;
    uint64_t best_rtt_time = 0;

    // Touchscreen -> PC (for second-screen apps). x/y are 0..1 over the video.
    void send_touch(uint8_t type, uint8_t finger, float x, float y)
    {
        std::lock_guard lock(mutex);
        if (!channel) return;
        proto::TouchMsg t{type, finger, 0, x, y};
        channel->send(proto::Msg::Touch, &t, sizeof(t));
    }

    void set_status(std::string s)
    {
        std::lock_guard lock(mutex);
        status = std::move(s);
    }

    void post_frame(AVFrame* f)
    {
        {
            std::lock_guard lock(mutex);
            if (latest) av_frame_free(&latest); // display is behind: drop the older one
            latest = f;
        }
        SDL_Event e{};
        e.type = frame_event;
        SDL_PushEvent(&e);
    }
};

bool discover_host(sockaddr_in& out, const std::atomic<bool>& quit)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(proto::kDiscoveryPort);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    proto::DiscoveryRequest req{proto::kMagic, proto::kVersion, 0};
    LOGI("looking for steamu-host on the local network...");
    while (!quit) {
        sendto(fd, &req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&bcast), sizeof(bcast));
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 1000) <= 0) continue;

        proto::DiscoveryReply reply{};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from),
                             &from_len);
        if (n != sizeof(reply) || reply.magic != proto::kMagic) continue;
        if (reply.version != proto::kVersion) {
            LOGW("host at %s speaks protocol v%u, we need v%u", net::to_string(from).c_str(),
                 reply.version, proto::kVersion);
            continue;
        }
        reply.hostname[sizeof(reply.hostname) - 1] = 0;
        out = from;
        out.sin_port = htons(reply.control_port);
        LOGI("found host '%s' at %s", reply.hostname, net::to_string(out).c_str());
        close(fd);
        return true;
    }
    close(fd);
    return false;
}

void copy_string(char* dst, size_t cap, const std::string& src)
{
    std::snprintf(dst, cap, "%s", src.c_str());
}

// Runs one connection to the host; returns when it drops.
void run_session(App& app, deck::HidBackend& hid, const sockaddr_in& host)
{
    int tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (tcp < 0) return;
    net::tune_tcp(tcp);
    if (connect(tcp, reinterpret_cast<const sockaddr*>(&host), sizeof(host)) < 0) {
        LOGW("connect to %s failed: %s", net::to_string(host).c_str(), strerror(errno));
        close(tcp);
        return;
    }
    net::Channel ch(tcp);

    // --- Handshake -------------------------------------------------------
    const deck::DeviceInfo& info = hid.info();
    auto hello = std::make_unique<proto::HelloMsg>();
    std::memset(hello.get(), 0, sizeof(*hello));
    hello->magic = proto::kMagic;
    hello->version = proto::kVersion;
    hello->bus = info.bus;
    hello->vendor = info.vendor;
    hello->product = info.product;
    hello->device_version = info.version;
    copy_string(hello->name, sizeof(hello->name), info.name);
    copy_string(hello->uniq, sizeof(hello->uniq), info.uniq);
    gethostname(hello->hostname, sizeof(hello->hostname) - 1);
    hello->descriptor_size = uint16_t(info.descriptor.size());
    std::memcpy(hello->descriptor, info.descriptor.data(), info.descriptor.size());

    const deck::UsbInfo& usb = info.usb;
    if (usb.valid && usb.descriptors.size() <= proto::kMaxUsbDescriptors &&
        usb.report_descriptors.size() <= proto::kMaxInterfaces) {
        hello->usb_valid = 1;
        hello->usb_speed = usb.speed;
        hello->gamepad_interface = usb.gamepad_interface;
        hello->interface_count = uint8_t(usb.report_descriptors.size());
        hello->usb_descriptors_size = uint16_t(usb.descriptors.size());
        std::memcpy(hello->usb_descriptors, usb.descriptors.data(), usb.descriptors.size());
        copy_string(hello->usb_manufacturer, sizeof(hello->usb_manufacturer), usb.manufacturer);
        copy_string(hello->usb_product, sizeof(hello->usb_product), usb.product);
        copy_string(hello->usb_serial, sizeof(hello->usb_serial), usb.serial);
        for (size_t i = 0; i < usb.report_descriptors.size(); ++i) {
            const auto& d = usb.report_descriptors[i];
            const size_t n = std::min(d.size(), proto::kMaxInterfaceDescriptor);
            hello->interface_descriptor_size[i] = uint16_t(n);
            std::memcpy(hello->interface_descriptor[i], d.data(), n);
        }
    }
    if (!ch.send(proto::Msg::Hello, hello.get(), sizeof(*hello))) return;

    proto::Msg type{};
    std::vector<uint8_t> payload;
    if (!ch.recv(type, payload) || type != proto::Msg::Welcome ||
        payload.size() < sizeof(proto::WelcomeMsg)) {
        LOGW("host rejected the handshake");
        return;
    }
    proto::WelcomeMsg welcome{};
    std::memcpy(&welcome, payload.data(), sizeof(welcome));
    LOGI("connected to %s, streaming controller", net::to_string(host).c_str());
    {
        std::lock_guard lock(app.mutex);
        app.connected = true;
        app.channel = &ch;
        app.status = "Connected to " + net::to_string(host) + " - waiting for video";
    }

    // --- Video ---------------------------------------------------------------
    deck::VideoReceiver video;
    if (app.opt.video) {
        const uint16_t port = video.start([&app](AVFrame* f) { app.post_frame(f); },
                                          [&ch] { ch.send(proto::Msg::RequestKeyframe, nullptr, 0); });
        if (port) {
            proto::VideoStartMsg v{port, uint16_t(app.display_w), uint16_t(app.display_h),
                                   uint16_t(app.refresh_hz)};
            ch.send(proto::Msg::VideoStart, &v, sizeof(v));
        } else {
            LOGE("cannot open the video socket");
        }
    }

    // --- Audio ----------------------------------------------------------------
    deck::AudioReceiver audio;
    if (app.opt.audio) {
        if (const uint16_t port = audio.start(app.opt.audio_buffer_ms)) {
            proto::AudioStartMsg a{port, 0};
            ch.send(proto::Msg::AudioStart, &a, sizeof(a));
        }
    }

    // --- Input stream (UDP) ----------------------------------------------
    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in input_addr = host;
    input_addr.sin_port = htons(welcome.input_port);
    net::tune_udp(udp);
    connect(udp, reinterpret_cast<sockaddr*>(&input_addr), sizeof(input_addr));

    std::atomic<bool> stop{false};
    std::thread input_thread([&] {
        uint8_t packet[sizeof(proto::InputPacketHeader) + 512];
        auto* hdr = reinterpret_cast<proto::InputPacketHeader*>(packet);
        uint8_t* report = packet + sizeof(*hdr);
        hdr->magic = proto::kMagic;
        hdr->token = welcome.session_token;
        hdr->reserved = 0;
        uint32_t seq = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            int n = hid.read_input(report, 512, 100);
            if (n < 0) {
                LOGE("controller read failed: %s", strerror(-n));
                ch.shutdown();
                return;
            }
            if (n == 0) continue;
            hdr->seq = seq++;
            hdr->size = uint16_t(n);
            hdr->deck_time_us = net::now_us();
            send(udp, packet, sizeof(*hdr) + size_t(n), MSG_DONTWAIT | MSG_NOSIGNAL);
        }
    });

    // --- Control requests from the PC ------------------------------------
    std::vector<uint8_t> buf(proto::kMaxReport);
    while (ch.recv(type, payload)) {
        switch (type) {
        case proto::Msg::GetReport: {
            if (payload.size() < sizeof(proto::GetReportMsg)) break;
            proto::GetReportMsg req{};
            std::memcpy(&req, payload.data(), sizeof(req));
            buf[0] = req.rnum;
            // The Deck's reports are 64 bytes + report number; ask for a
            // little more so other devices also fit.
            int r = hid.get_report(req.rtype, buf.data(), 65);
            proto::ReportReplyMsg reply{req.id, r < 0 ? -r : 0};
            ch.send(proto::Msg::GetReportReply, &reply, sizeof(reply), buf.data(),
                    r < 0 ? 0 : size_t(r));
            LOGD("get_report rtype=%u rnum=%u -> %d", req.rtype, req.rnum, r);
            break;
        }
        case proto::Msg::SetReport: {
            if (payload.size() < sizeof(proto::SetReportMsg)) break;
            proto::SetReportMsg req{};
            std::memcpy(&req, payload.data(), sizeof(req));
            const uint8_t* data = payload.data() + sizeof(req);
            size_t size = payload.size() - sizeof(req);
            int r = hid.set_report(req.rtype, data, size);
            proto::ReportReplyMsg reply{req.id, r < 0 ? -r : 0};
            ch.send(proto::Msg::SetReportReply, &reply, sizeof(reply));
            LOGD("set_report rtype=%u cmd=0x%02x size=%zu -> %d", req.rtype,
                 size > 1 ? data[1] : 0, size, r);
            break;
        }
        case proto::Msg::OutputReport: {
            if (payload.size() < sizeof(proto::OutputReportMsg)) break;
            hid.write_output(payload.data() + sizeof(proto::OutputReportMsg),
                             payload.size() - sizeof(proto::OutputReportMsg));
            break;
        }
        case proto::Msg::Ping: {
            ch.send(proto::Msg::Pong, payload.data(), payload.size());
            if (payload.size() < sizeof(proto::PingMsg)) break;
            proto::PingMsg ping{};
            std::memcpy(&ping, payload.data(), sizeof(ping));
            if (!ping.last_rtt_us) break;
            // The ping left the host about rtt/2 ago (in host time).
            const uint64_t now = net::now_us();
            if (ping.last_rtt_us <= app.best_rtt_us || now - app.best_rtt_time > 30000000) {
                app.best_rtt_us = ping.last_rtt_us;
                app.best_rtt_time = now;
                app.clock_offset_us =
                    int64_t(now) - int64_t(ping.host_time_us + ping.last_rtt_us / 2);
                app.clock_valid = true;
            }
            break;
        }
        default:
            LOGW("unexpected message type %u", unsigned(type));
            break;
        }
    }

    LOGW("connection to host lost");
    video.stop();
    audio.stop();
    stop = true;
    input_thread.join();
    close(udp);
    std::lock_guard lock(app.mutex);
    app.connected = false;
    app.channel = nullptr;
    if (app.latest) av_frame_free(&app.latest);
}

void network_thread(App& app, deck::HidBackend& hid)
{
    while (!app.quit) {
        sockaddr_in host{};
        if (!app.opt.host.empty()) {
            if (!net::parse_ipv4(app.opt.host.c_str(), app.opt.port, host)) {
                app.set_status("Invalid --host address: " + app.opt.host);
                return;
            }
        } else {
            app.set_status("Searching for steamu-host on the network...");
            if (!discover_host(host, app.quit)) return;
        }
        app.set_status("Connecting to " + net::to_string(host) + "...");
        run_session(app, hid, host);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Display ---------------------------------------------------------------------

struct Display {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_w = 0, tex_h = 0, tex_format = -1;
    SDL_FRect video_rect{0, 0, 1, 1}; // where the picture is, in render pixels

    bool upload(const AVFrame* f)
    {
        if (f->format != AV_PIX_FMT_NV12 && f->format != AV_PIX_FMT_YUV420P) {
            LOGE("unsupported frame format %d", f->format);
            return false;
        }
        if (!texture || tex_w != f->width || tex_h != f->height || tex_format != f->format) {
            if (texture) SDL_DestroyTexture(texture);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                  f->format == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12
                                                               : SDL_PIXELFORMAT_IYUV);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                  SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                  SDL_COLORSPACE_BT709_LIMITED);
            texture = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            if (!texture) {
                LOGE("cannot create video texture: %s", SDL_GetError());
                return false;
            }
            tex_w = f->width;
            tex_h = f->height;
            tex_format = f->format;
        }
        if (f->format == AV_PIX_FMT_NV12)
            return SDL_UpdateNVTexture(texture, nullptr, f->data[0], f->linesize[0], f->data[1],
                                       f->linesize[1]);
        return SDL_UpdateYUVTexture(texture, nullptr, f->data[0], f->linesize[0], f->data[1],
                                    f->linesize[1], f->data[2], f->linesize[2]);
    }

    void draw_video()
    {
        int out_w = 0, out_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
        // Letterbox to keep the aspect ratio.
        const float scale = std::min(float(out_w) / float(tex_w), float(out_h) / float(tex_h));
        SDL_FRect dst{(float(out_w) - float(tex_w) * scale) / 2,
                      (float(out_h) - float(tex_h) * scale) / 2, float(tex_w) * scale,
                      float(tex_h) * scale};
        video_rect = dst;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, &dst);
    }

    // Window-normalized (0..1) -> video-normalized (0..1); false if outside.
    bool to_video(float wx, float wy, float& vx, float& vy) const
    {
        int out_w = 0, out_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
        vx = (wx * float(out_w) - video_rect.x) / video_rect.w;
        vy = (wy * float(out_h) - video_rect.y) / video_rect.h;
        return vx >= 0 && vx <= 1 && vy >= 0 && vy <= 1;
    }

    // Quit button on the status screen (render coordinates), so the app
    // can be closed without a keyboard, e.g. in Desktop Mode.
    SDL_FRect quit_button() const
    {
        int w = 1280, h = 800;
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
        const float bw = 240, bh = 96;
        return {float(w) - bw - 32, float(h) - bh - 32, bw, bh};
    }

    void draw_status(const std::string& text, bool quit_pressed)
    {
        SDL_SetRenderDrawColor(renderer, 16, 20, 28, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderScale(renderer, 2.0f, 2.0f);
        SDL_SetRenderDrawColor(renderer, 220, 225, 235, 255);
        SDL_RenderDebugText(renderer, 16, 16, "SteamU");
        SDL_SetRenderDrawColor(renderer, 150, 160, 175, 255);
        SDL_RenderDebugText(renderer, 16, 36, text.c_str());
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);

        const SDL_FRect b = quit_button();
        if (quit_pressed) SDL_SetRenderDrawColor(renderer, 150, 50, 50, 255);
        else SDL_SetRenderDrawColor(renderer, 90, 34, 38, 255);
        SDL_RenderFillRect(renderer, &b);
        SDL_SetRenderDrawColor(renderer, 200, 90, 90, 255);
        SDL_RenderRect(renderer, &b);
        // Debug font: 8x8 per character, drawn at 3x.
        SDL_SetRenderScale(renderer, 3.0f, 3.0f);
        SDL_SetRenderDrawColor(renderer, 240, 235, 235, 255);
        SDL_RenderDebugText(renderer, (b.x + b.w / 2 - 4 * 8 * 3 / 2) / 3, (b.y + b.h / 2 - 12) / 3,
                            "Quit");
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    }
};

} // namespace

int main(int argc, char** argv)
{
    static App app; // static: the network thread may outlive main's scope at exit
    Options& opt = app.opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) opt.host = argv[++i];
        else if (a == "--hidraw" && i + 1 < argc) opt.hidraw = argv[++i];
        else if (a == "--fake") opt.fake = true;
        else if (a == "--fake-idle") opt.fake = opt.fake_idle = true;
        else if (a == "--fake-stdin") opt.fake = opt.fake_idle = opt.fake_stdin = true;
        else if (a == "--windowed") opt.windowed = true;
        else if (a == "--vsync") opt.vsync = true;
        else if (a == "--no-video") opt.video = false;
        else if (a == "--no-audio") opt.audio = false;
        else if (a == "--audio-buffer" && i + 1 < argc) opt.audio_buffer_ms = std::atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) opt.port = uint16_t(std::atoi(argv[++i]));
        else if (a == "-v" || a == "--verbose") g_verbose = true;
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    // Touch is forwarded as touch; don't also turn it into mouse clicks.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "SteamU");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "240"); // 5 ms device buffer
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (opt.audio && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGW("no audio: %s", SDL_GetError());
        opt.audio = false;
    }
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        if (!opt.windowed) {
            app.display_w = mode->w;
            app.display_h = mode->h;
        }
        if (mode->refresh_rate > 1) app.refresh_hz = int(mode->refresh_rate + 0.5f);
    }
    app.frame_event = SDL_RegisterEvents(1);

    Display disp;
    const SDL_WindowFlags flags = opt.windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN;
    if (!SDL_CreateWindowAndRenderer("SteamU", app.display_w, app.display_h, flags, &disp.window,
                                     &disp.renderer)) {
        LOGE("cannot create window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(disp.renderer, opt.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    if (!opt.windowed) SDL_HideCursor();
    LOGI("display %dx%d @ %d Hz, renderer %s", app.display_w, app.display_h, app.refresh_hz,
         SDL_GetRendererName(disp.renderer));

    auto hid = opt.fake ? deck::open_fake_backend(opt.fake_idle) : deck::open_hidraw_backend(opt.hidraw);
    if (hid && opt.fake_stdin) {
        std::thread([&hid] {
            std::string cmd, button;
            while (std::cin >> cmd >> button) {
                const bool ok = cmd == "release" ? deck::fake_press(*hid, button, false)
                                                 : deck::fake_press(*hid, button, true);
                if (!ok) LOGW("fake-stdin: unknown button '%s'", button.c_str());
                if (cmd == "tap") {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    deck::fake_press(*hid, button, false);
                }
                LOGD("fake-stdin: %s %s", cmd.c_str(), button.c_str());
            }
        }).detach();
    }
    if (hid) {
        std::thread(network_thread, std::ref(app), std::ref(*hid)).detach();
    } else {
        app.set_status("No Steam Deck controller found (use --fake to test without one)");
    }

    // Touch forwarding. Real fingers keep their own slot (0..9); a mouse (or
    // Gamescope's touch-as-mouse mode) acts as finger 0.
    SDL_FingerID finger_ids[10] = {};
    bool finger_used[10] = {};
    bool mouse_down = false;
    auto finger_slot = [&](SDL_FingerID id, bool create) -> int {
        for (int i = 0; i < 10; ++i)
            if (finger_used[i] && finger_ids[i] == id) return i;
        if (!create) return -1;
        for (int i = 0; i < 10; ++i)
            if (!finger_used[i]) {
                finger_used[i] = true;
                finger_ids[i] = id;
                return i;
            }
        return -1;
    };
    auto handle_touch = [&](const SDL_Event& ev) {
        float vx, vy;
        switch (ev.type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            const bool down = ev.type == SDL_EVENT_FINGER_DOWN, up = ev.type == SDL_EVENT_FINGER_UP;
            const int slot = finger_slot(ev.tfinger.fingerID, down);
            if (slot < 0) return;
            disp.to_video(ev.tfinger.x, ev.tfinger.y, vx, vy);
            app.send_touch(down ? 1 : up ? 3 : 2, uint8_t(slot), std::clamp(vx, 0.f, 1.f),
                           std::clamp(vy, 0.f, 1.f));
            if (up) finger_used[slot] = false;
            return;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION: {
            if (ev.motion.which == SDL_TOUCH_MOUSEID) return; // synthesized from touch
            const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const bool up = ev.type == SDL_EVENT_MOUSE_BUTTON_UP;
            if (!down && !up && !mouse_down) return; // hover
            int ww = 1, wh = 1;
            SDL_GetWindowSize(disp.window, &ww, &wh);
            const float mx = down || up ? ev.button.x : ev.motion.x;
            const float my = down || up ? ev.button.y : ev.motion.y;
            disp.to_video(mx / float(ww), my / float(wh), vx, vy);
            mouse_down = down ? true : up ? false : mouse_down;
            app.send_touch(down ? 1 : up ? 3 : 2, 0, std::clamp(vx, 0.f, 1.f),
                           std::clamp(vy, 0.f, 1.f));
            return;
        }
        default: return;
        }
    };

    bool showing_video = false;
    bool quit_pressed = false; // finger/mouse went down on the Quit button
    // Quit button: press and release on it (status screen only).
    auto handle_quit_button = [&](const SDL_Event& ev) {
        SDL_Event e = ev;
        float x, y;
        bool down;
        if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP) {
            SDL_ConvertEventToRenderCoordinates(disp.renderer, &e);
            x = e.tfinger.x;
            y = e.tfinger.y;
            down = e.type == SDL_EVENT_FINGER_DOWN;
        } else if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                   e.button.button == SDL_BUTTON_LEFT) {
            SDL_ConvertEventToRenderCoordinates(disp.renderer, &e);
            x = e.button.x;
            y = e.button.y;
            down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        } else {
            return;
        }
        const SDL_FRect b = disp.quit_button();
        const bool inside = x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
        if (down) quit_pressed = inside;
        else {
            if (quit_pressed && inside) app.quit = true;
            quit_pressed = false;
        }
    };
    uint64_t stat_start = net::now_us(), stat_frames = 0, stat_latency = 0, stat_latency_n = 0;
    while (!app.quit) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 250)) e.type = 0;
        do {
            if (e.type == SDL_EVENT_QUIT) app.quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && opt.windowed)
                app.quit = true;
            if (showing_video) handle_touch(e);
            else handle_quit_button(e);
        } while (SDL_PollEvent(&e));
        if (app.quit) break;

        AVFrame* frame = nullptr;
        std::string status;
        bool connected;
        {
            std::lock_guard lock(app.mutex);
            frame = app.latest;
            app.latest = nullptr;
            status = app.status;
            connected = app.connected;
        }
        if (!connected) showing_video = false;

        if (frame) {
            if (disp.upload(frame)) {
                showing_video = true;
                disp.draw_video();
                SDL_RenderPresent(disp.renderer);
                ++stat_frames;
                if (app.clock_valid) {
                    const int64_t capture_deck = frame->pts + app.clock_offset_us.load();
                    const int64_t latency = int64_t(net::now_us()) - capture_deck;
                    if (latency > 0 && latency < 1000000) {
                        stat_latency += uint64_t(latency);
                        ++stat_latency_n;
                    }
                }
            }
            av_frame_free(&frame);
        } else if (!showing_video) {
            disp.draw_status(status, quit_pressed);
            SDL_RenderPresent(disp.renderer);
        }

        const uint64_t now = net::now_us();
        if (now - stat_start >= 5000000) {
            if (stat_frames)
                LOGI("display: %.0f fps, capture->present %.1f ms",
                     double(stat_frames) * 1e6 / double(now - stat_start),
                     stat_latency_n ? double(stat_latency) / double(stat_latency_n) / 1000 : 0.0);
            stat_start = now;
            stat_frames = stat_latency = stat_latency_n = 0;
        }
    }

    // Only video: the network thread may still be feeding the audio streams,
    // and the process ends right after anyway.
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    std::fflush(stderr);
    _exit(0); // network threads may be blocked in recv; just leave
}
