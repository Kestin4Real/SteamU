// Receives the video stream: reassembles UDP chunks into frames, decodes them
// (VAAPI on the Deck, software fallback elsewhere) and hands each decoded
// frame to a sink. Lost frames trigger a keyframe request instead of
// showing a corrupted picture.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

struct AVFrame;

namespace steamu::deck {

class VideoReceiver {
public:
    // Called on the receive thread. The sink takes ownership of the frame.
    // frame->pts holds the host capture time (host clock, microseconds).
    using FrameSink = std::function<void(AVFrame* frame)>;
    using KeyframeRequest = std::function<void()>;

    VideoReceiver() = default;
    ~VideoReceiver();
    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // Binds a UDP port and starts the receive thread. Returns the port or 0.
    uint16_t start(FrameSink sink, KeyframeRequest request_keyframe);
    void stop();

private:
    void run();

    int sock_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    FrameSink sink_;
    KeyframeRequest request_keyframe_;
};

} // namespace steamu::deck
