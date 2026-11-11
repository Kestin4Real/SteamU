// Host side of the second-screen API (sdk/steamu_screen.h): one local app at
// a time can attach and replace the mirrored monitor with its own frames,
// and receives the Deck's touch input.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace steamu::net {
class Channel;
}

namespace steamu::host {

class ScreenServer {
public:
    struct Mapping {
        void* base = nullptr;
        size_t size = 0;
        ~Mapping();
    };

    // A frame borrowed from the client. Keeps the mapping alive; hand it
    // back with release() once the pixels have been read.
    struct Frame {
        std::shared_ptr<Mapping> mapping;
        const uint8_t* data = nullptr;
        int width = 0, height = 0, stride = 0;
        uint32_t format = 0; // sus_format
        bool flip_y = false;
        uint32_t slot = 0;
        uint64_t generation = 0; // changes on every (re)attach
        uint64_t time_us = 0;
    };

    ScreenServer() = default;
    ~ScreenServer();

    bool start();

    // Deck state, forwarded to the client so it can render at the right size.
    void set_display(bool connected, int width, int height, int refresh_hz);
    void send_touch(uint8_t type, uint8_t finger, float x, float y);

    // Every raw Steam Deck input report passes through here. Forwards it to
    // a subscribed client. Returns true if that client wants the controls
    // exclusively (the virtual PC controller must then stay neutral).
    bool controller_report(const uint8_t* report, size_t size);

    // True while a client is attached (the Deck shows its frames).
    bool active();
    // Waits up to timeout_ms for a new frame. False on timeout/inactive.
    bool wait_frame(int timeout_ms, Frame& out);
    void release(const Frame& frame);

    // App audio for the Deck only (48 kHz stereo s16 interleaved). Waits up
    // to timeout_ms until `frames` frames are buffered and moves them to out.
    bool take_audio(int16_t* out, size_t frames, int timeout_ms);
    // True while a connected client asked to silence the desktop audio.
    bool desktop_audio_muted();

private:
    void push_audio(const uint8_t* data, size_t size);

    void accept_loop();
    void serve(int fd);
    void detach_locked();
    void send_display_locked();

    std::string path_;
    int listen_fd_ = -1;
    unsigned long socket_inode_ = 0;
    std::thread thread_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<net::Channel> client_;
    std::string client_name_;

    // Attachment
    std::shared_ptr<Mapping> mapping_;
    int width_ = 0, height_ = 0, stride_ = 0, slots_ = 0;
    uint32_t format_ = 0;
    bool flip_y_ = false;
    uint64_t generation_ = 0;

    // Newest submitted frame not yet taken by the streamer.
    bool pending_ = false;
    uint32_t pending_slot_ = 0;
    uint64_t pending_time_ = 0;

    // Controls
    bool subscribed_ = false;
    bool exclusive_ = false;

    // App audio
    std::mutex audio_mutex_;
    std::condition_variable audio_cv_;
    std::vector<int16_t> audio_;   // interleaved stereo
    uint64_t audio_dropped_ = 0;   // frames dropped because the client ran ahead
    bool mute_desktop_ = false;    // guarded by mutex_

    // Deck display
    bool connected_ = false;
    int display_w_ = 1280, display_h_ = 800, display_hz_ = 60;
};

} // namespace steamu::host
