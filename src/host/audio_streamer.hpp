// Audio to the Deck, as Opus over UDP (48 kHz stereo):
//  * Desktop: whatever the PC plays, captured from the default output's
//    monitor (PulseAudio / PipeWire-Pulse).
//  * App: Deck-only sound from a second-screen app (steamu_screen.h).
// Each stream has its own thread and encoder; the Deck mixes them.
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace steamu::host {

class ScreenServer;

struct AudioOptions {
    bool desktop = true;   // stream the PC's sound
    std::string source;    // PulseAudio source, empty = default output's monitor
    int bitrate_kbps = 128;
    int frame_ms = 5;      // Opus frame: 5, 10 or 20 ms (smaller = less delay)
};

class AudioStreamer {
public:
    AudioStreamer() = default;
    ~AudioStreamer();
    AudioStreamer(const AudioStreamer&) = delete;
    AudioStreamer& operator=(const AudioStreamer&) = delete;

    void start(const AudioOptions& options, const sockaddr_in& deck, ScreenServer* screens);
    void stop();

    struct Shared; // outlives a capture thread stuck in a blocking read

private:
    std::shared_ptr<Shared> shared_;
    std::thread app_thread_;
};

} // namespace steamu::host
