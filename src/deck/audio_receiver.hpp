// Receives the PC's audio streams (desktop + second-screen app), decodes the
// Opus packets and plays them with SDL, which mixes the streams.
//
// Wi-Fi delivers packets unevenly, so each stream keeps a small buffer
// (--audio-buffer, default 30 ms) in front of the sound card. The two
// machines' clocks also run at slightly different speeds; instead of
// letting the buffer slowly fill up or run dry, playback speed is nudged by
// at most 0.5% to hold it at the target. After a gap (the PC stops sending
// during silence, or packets were lost) it re-buffers with silence first.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace steamu::deck {

class AudioReceiver {
public:
    AudioReceiver() = default;
    ~AudioReceiver();
    AudioReceiver(const AudioReceiver&) = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    // Opens the audio output and a UDP port. Returns the port, or 0.
    uint16_t start(int buffer_ms);
    void stop();

private:
    void run();

    int sock_ = -1;
    uint32_t device_ = 0;
    int buffer_ms_ = 30;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace steamu::deck
