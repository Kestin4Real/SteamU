// Sends a copy of the Deck's video to a local recorder (OBS Media Source)
// as MPEG-TS over UDP. The already-encoded packets are only re-wrapped,
// so this costs no extra encoding and doesn't affect the Deck stream.
#pragma once

#include <cstdint>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace steamu::host {

class ObsOutput {
public:
    ObsOutput() = default;
    ~ObsOutput();
    ObsOutput(const ObsOutput&) = delete;
    ObsOutput& operator=(const ObsOutput&) = delete;

    // url e.g. "udp://127.0.0.1:27860?pkt_size=1316". enc = the opened encoder.
    bool open(const std::string& url, const AVCodecContext* enc);
    void close();
    bool active() const { return fmt_ != nullptr; }

    // capture_us: when the frame was captured (CLOCK_MONOTONIC), used as timestamp.
    void write(const AVPacket* pkt, uint64_t capture_us);

private:
    AVFormatContext* fmt_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* pkt_ = nullptr;
    uint64_t base_us_ = 0;
    int64_t last_pts_ = -1;
    std::string url_;
};

} // namespace steamu::host
