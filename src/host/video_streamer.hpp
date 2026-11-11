// Video to the Deck: monitor mirror (capture -> crop/scale) or frames from a
// second-screen app, then NVENC -> UDP. Runs on its own thread.
#pragma once

#include "common/protocol.hpp"

#include <netinet/in.h>

#include <atomic>
#include <string>
#include <thread>

namespace steamu::host {

class ScreenServer;

struct VideoOptions {
    std::string monitor;          // RandR name, empty = primary
    std::string codec = "hevc";   // hevc | h264
    int bitrate_kbps = 20000; // plenty for 1280x800 HEVC
    int fps = 0;                  // 0 = Deck's refresh rate
    std::string obs_url;          // non-empty: also send the video here (MPEG-TS)
};

class VideoStreamer {
public:
    VideoStreamer() = default;
    ~VideoStreamer();
    VideoStreamer(const VideoStreamer&) = delete;
    VideoStreamer& operator=(const VideoStreamer&) = delete;

    // screens: optional second-screen server; while an app is attached its
    // frames are streamed instead of the monitor.
    void start(const VideoOptions& options, const sockaddr_in& deck, int width, int height,
               int refresh_hz, ScreenServer* screens);
    void stop();
    void request_keyframe() { keyframe_requested_ = true; }

private:
    void run(VideoOptions options, sockaddr_in deck, int width, int height, int fps);

    std::thread thread_;
    ScreenServer* screens_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keyframe_requested_{true};
};

} // namespace steamu::host
