#include "host/screen_server.hpp"

#include "common/log.hpp"
#include "common/net.hpp"

#include "steamu_screen.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace steamu::host {

ScreenServer::Mapping::~Mapping()
{
    if (base) munmap(base, size);
}

ScreenServer::~ScreenServer()
{
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        // Only remove the socket file if it is still ours.
        struct stat st{};
        if (stat(path_.c_str(), &st) == 0 && st.st_ino == socket_inode_) unlink(path_.c_str());
    }
    if (thread_.joinable()) thread_.detach();
}

bool ScreenServer::start()
{
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir) {
        LOGW("XDG_RUNTIME_DIR not set: second-screen API disabled");
        return false;
    }
    // STEAMU_SCREEN_SOCKET lets a test instance use its own socket.
    const char* override_path = std::getenv("STEAMU_SCREEN_SOCKET");
    path_ = override_path && *override_path ? override_path : std::string(dir) + "/" + SUS_SOCKET_NAME;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) {
        LOGE("second-screen socket path too long (max %zu): %s", sizeof(addr.sun_path) - 1,
             path_.c_str());
        return false;
    }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);

    // Never take the socket away from another running host: only replace
    // it if nobody is listening on it anymore (left over from a crash).
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const bool live = probe >= 0 && connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (probe >= 0) ::close(probe);
    if (live) {
        LOGW("another steamu-host already serves %s: second-screen API disabled in this "
             "instance (set STEAMU_SCREEN_SOCKET to use a different socket)",
             path_.c_str());
        return false;
    }
    unlink(path_.c_str()); // stale socket from a previous run

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0 || bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listen_fd_, 2) < 0) {
        LOGE("cannot create %s: %s", path_.c_str(), strerror(errno));
        if (listen_fd_ >= 0) ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    chmod(path_.c_str(), 0600);
    struct stat st{};
    if (stat(path_.c_str(), &st) == 0) socket_inode_ = st.st_ino;
    thread_ = std::thread([this] { accept_loop(); });
    LOGI("second-screen API at %s", path_.c_str());
    return true;
}

void ScreenServer::accept_loop()
{
    for (;;) {
        int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return; // shut down
        }
        // Only the same user may connect (the socket is 0600 in a 0700 dir,
        // this is belt and braces).
        ucred cred{};
        socklen_t len = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != getuid()) {
            ::close(fd);
            continue;
        }
        bool busy;
        {
            std::lock_guard lock(mutex_);
            busy = client_ != nullptr;
        }
        if (busy) {
            LOGW("second-screen: another app is already connected, refusing");
            ::close(fd);
            continue;
        }
        std::thread([this, fd] { serve(fd); }).detach();
    }
}

void ScreenServer::send_display_locked()
{
    if (!client_) return;
    sus_display_msg d{};
    d.connected = connected_ ? 1 : 0;
    d.width = uint16_t(display_w_);
    d.height = uint16_t(display_h_);
    d.refresh_hz = uint16_t(display_hz_);
    client_->send(proto::Msg(SUS_MSG_DISPLAY), &d, sizeof(d));
}

void ScreenServer::detach_locked()
{
    if (mapping_) LOGI("second-screen: '%s' detached, back to mirroring", client_name_.c_str());
    mapping_.reset(); // frames in flight keep their own reference
    pending_ = false;
    ++generation_;
    cv_.notify_all();
}

void ScreenServer::serve(int fd)
{
    auto channel = std::make_shared<net::Channel>(fd);
    proto::Msg type{};
    std::vector<uint8_t> payload;

    if (!channel->recv(type, payload) || uint16_t(type) != SUS_MSG_HELLO ||
        payload.size() < sizeof(sus_hello_msg)) {
        return;
    }
    sus_hello_msg hello{};
    std::memcpy(&hello, payload.data(), sizeof(hello));
    if (hello.magic != SUS_MAGIC || hello.version != SUS_VERSION) {
        LOGW("second-screen: client speaks v%u, we are v%u", hello.version, SUS_VERSION);
        return;
    }
    hello.app_name[sizeof(hello.app_name) - 1] = 0;
    {
        std::lock_guard lock(mutex_);
        if (client_) return;
        client_ = channel;
        client_name_ = hello.app_name;
        send_display_locked();
    }
    LOGI("second-screen: '%s' connected", hello.app_name);

    while (channel->recv(type, payload)) {
        switch (uint16_t(type)) {
        case SUS_MSG_ATTACH: {
            if (payload.size() < sizeof(sus_attach_msg)) break;
            sus_attach_msg a{};
            std::memcpy(&a, payload.data(), sizeof(a));
            a.shm_path[sizeof(a.shm_path) - 1] = 0;
            const size_t slot_size = size_t(a.stride) * a.height;
            if (std::strncmp(a.shm_path, "/dev/shm/", 9) != 0 || std::strstr(a.shm_path, "..") ||
                a.width < 16 || a.height < 16 || a.width > 4096 || a.height > 4096 ||
                a.stride < a.width * 4 || a.slot_count < 1 || a.slot_count > SUS_MAX_SLOTS ||
                (a.format != SUS_FORMAT_RGBA8 && a.format != SUS_FORMAT_BGRA8)) {
                LOGW("second-screen: invalid ATTACH");
                break;
            }
            int shm = ::open(a.shm_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat st{};
            const size_t total = slot_size * a.slot_count;
            if (shm < 0 || fstat(shm, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
                size_t(st.st_size) < total) {
                LOGW("second-screen: cannot use %s", a.shm_path);
                if (shm >= 0) ::close(shm);
                break;
            }
            void* base = mmap(nullptr, total, PROT_READ, MAP_SHARED, shm, 0);
            ::close(shm);
            if (base == MAP_FAILED) break;
            auto mapping = std::make_shared<Mapping>();
            mapping->base = base;
            mapping->size = total;

            std::lock_guard lock(mutex_);
            detach_locked();
            mapping_ = mapping;
            width_ = int(a.width);
            height_ = int(a.height);
            stride_ = int(a.stride);
            slots_ = int(a.slot_count);
            format_ = a.format;
            flip_y_ = a.flip_y != 0;
            LOGI("second-screen: '%s' attached %ux%u, %u slots%s", client_name_.c_str(), a.width,
                 a.height, a.slot_count, flip_y_ ? ", bottom-up" : "");
            break;
        }
        case SUS_MSG_FRAME: {
            if (payload.size() < sizeof(sus_frame_msg)) break;
            sus_frame_msg f{};
            std::memcpy(&f, payload.data(), sizeof(f));
            std::lock_guard lock(mutex_);
            if (!mapping_ || f.slot >= uint32_t(slots_)) break;
            if (pending_) {
                // The streamer never took the previous frame: it is
                // superseded, give its slot straight back.
                sus_release_msg r{pending_slot_};
                client_->send(proto::Msg(SUS_MSG_RELEASE), &r, sizeof(r));
            }
            pending_ = true;
            pending_slot_ = f.slot;
            pending_time_ = f.time_us;
            cv_.notify_all();
            break;
        }
        case SUS_MSG_INPUT_MODE: {
            if (payload.size() < sizeof(sus_input_mode_msg)) break;
            sus_input_mode_msg m{};
            std::memcpy(&m, payload.data(), sizeof(m));
            std::lock_guard lock(mutex_);
            const bool sub = m.subscribe != 0, excl = sub && m.exclusive != 0;
            if (sub != subscribed_ || excl != exclusive_)
                LOGI("second-screen: '%s' %s the controls%s", client_name_.c_str(),
                     sub ? "receives" : "no longer receives", excl ? " (exclusive)" : "");
            subscribed_ = sub;
            exclusive_ = excl;
            break;
        }
        case SUS_MSG_AUDIO:
            push_audio(payload.data(), payload.size());
            break;
        case SUS_MSG_AUDIO_MODE: {
            if (payload.size() < sizeof(sus_audio_mode_msg)) break;
            sus_audio_mode_msg m{};
            std::memcpy(&m, payload.data(), sizeof(m));
            std::lock_guard lock(mutex_);
            if ((m.mute_desktop != 0) != mute_desktop_)
                LOGI("second-screen: '%s' %s the desktop audio on the Deck", client_name_.c_str(),
                     m.mute_desktop ? "mutes" : "unmutes");
            mute_desktop_ = m.mute_desktop != 0;
            break;
        }
        case SUS_MSG_DETACH: {
            std::lock_guard lock(mutex_);
            detach_locked();
            break;
        }
        default: break;
        }
    }

    std::lock_guard lock(mutex_);
    detach_locked();
    client_.reset();
    subscribed_ = exclusive_ = false;
    mute_desktop_ = false;
    {
        std::lock_guard audio_lock(audio_mutex_);
        audio_.clear(); // don't play a gone client's leftovers
    }
    LOGI("second-screen: '%s' disconnected", client_name_.c_str());
}

void ScreenServer::set_display(bool connected, int width, int height, int refresh_hz)
{
    std::lock_guard lock(mutex_);
    connected_ = connected;
    if (width > 0) display_w_ = width;
    if (height > 0) display_h_ = height;
    if (refresh_hz > 0) display_hz_ = refresh_hz;
    send_display_locked();
}

void ScreenServer::send_touch(uint8_t type, uint8_t finger, float x, float y)
{
    std::lock_guard lock(mutex_);
    if (!client_ || !mapping_) return;
    sus_touch_msg t{};
    t.type = type;
    t.finger = finger;
    t.x = x;
    t.y = y;
    client_->send(proto::Msg(SUS_MSG_TOUCH), &t, sizeof(t));
}

bool ScreenServer::controller_report(const uint8_t* r, size_t size)
{
    // Steam Deck state report: version 0x0001, type 0x09, 64 bytes. Layout
    // per SDL's SteamDeckStatePacket_t.
    if (size < 60 || r[2] != 0x09) return false;
    std::shared_ptr<net::Channel> client;
    bool exclusive;
    {
        std::lock_guard lock(mutex_);
        if (!client_ || !subscribed_) return false;
        client = client_;
        exclusive = exclusive_;
    }
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, r + o, 4); return v; };
    auto s16 = [&](size_t o) { int16_t v; std::memcpy(&v, r + o, 2); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, r + o, 2); return v; };
    sus_controller_msg c{};
    c.seq = u32(4);
    c.buttons_lo = u32(8);
    c.buttons_hi = u32(12);
    c.left_pad_x = s16(16);
    c.left_pad_y = s16(18);
    c.right_pad_x = s16(20);
    c.right_pad_y = s16(22);
    // Raw IMU axes -> SDL convention (x right, y up, z towards the player).
    c.accel[0] = s16(24);
    c.accel[1] = s16(28);
    c.accel[2] = int16_t(-std::max<int>(s16(26), -32767));
    c.gyro[0] = s16(30);
    c.gyro[1] = s16(34);
    c.gyro[2] = int16_t(-std::max<int>(s16(32), -32767));
    c.left_trigger = u16(44);
    c.right_trigger = u16(46);
    c.left_x = s16(48);
    c.left_y = s16(50);
    c.right_x = s16(52);
    c.right_y = s16(54);
    c.left_pad_pressure = u16(56);
    c.right_pad_pressure = u16(58);
    client->try_send(proto::Msg(SUS_MSG_CONTROLLER), &c, sizeof(c));
    return exclusive;
}

bool ScreenServer::active()
{
    std::lock_guard lock(mutex_);
    return mapping_ != nullptr;
}

bool ScreenServer::wait_frame(int timeout_ms, Frame& out)
{
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return pending_ || !mapping_; });
    if (!pending_ || !mapping_) return false;
    pending_ = false;
    out.mapping = mapping_;
    out.width = width_;
    out.height = height_;
    out.stride = stride_;
    out.format = format_;
    out.flip_y = flip_y_;
    out.slot = pending_slot_;
    out.generation = generation_;
    out.time_us = pending_time_;
    out.data = static_cast<const uint8_t*>(mapping_->base) + size_t(pending_slot_) * size_t(stride_) *
                                                                 size_t(height_);
    return true;
}

void ScreenServer::push_audio(const uint8_t* data, size_t size)
{
    constexpr size_t kMaxFrames = 48000 / 5; // 200 ms
    const size_t frames = size / 4;
    if (!frames) return;
    std::lock_guard lock(audio_mutex_);
    const size_t old = audio_.size();
    audio_.resize(old + frames * 2);
    std::memcpy(audio_.data() + old, data, frames * 4);
    if (audio_.size() > kMaxFrames * 2) { // client is ahead of real time: keep the newest
        const size_t drop = audio_.size() - kMaxFrames * 2;
        audio_.erase(audio_.begin(), audio_.begin() + std::ptrdiff_t(drop));
        if (audio_dropped_ == 0) LOGW("second-screen: app audio is ahead of real time, dropping");
        audio_dropped_ += drop / 2;
    }
    audio_cv_.notify_all();
}

bool ScreenServer::take_audio(int16_t* out, size_t frames, int timeout_ms)
{
    std::unique_lock lock(audio_mutex_);
    if (!audio_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return audio_.size() >= frames * 2; }))
        return false;
    std::memcpy(out, audio_.data(), frames * 4);
    audio_.erase(audio_.begin(), audio_.begin() + std::ptrdiff_t(frames * 2));
    return true;
}

bool ScreenServer::desktop_audio_muted()
{
    std::lock_guard lock(mutex_);
    return mute_desktop_;
}

void ScreenServer::release(const Frame& frame)
{
    std::lock_guard lock(mutex_);
    if (!client_ || frame.generation != generation_) return;
    sus_release_msg r{frame.slot};
    client_->send(proto::Msg(SUS_MSG_RELEASE), &r, sizeof(r));
}

} // namespace steamu::host
