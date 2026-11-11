#include "host/audio_streamer.hpp"

#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"
#include "host/screen_server.hpp"

#include <opus.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace steamu::host {

struct AudioStreamer::Shared {
    AudioOptions options;
    sockaddr_in deck{};
    ScreenServer* screens = nullptr;
    std::atomic<bool> stop{false};

    std::mutex mutex;
    std::condition_variable cv;
    bool desktop_done = false;
};

namespace {

class OpusSender {
public:
    OpusSender(proto::AudioStream stream, const sockaddr_in& deck, int bitrate_kbps)
        : stream_(stream)
    {
        int err = 0;
        enc_ = opus_encoder_create(proto::kAudioRate, proto::kAudioChannels,
                                   OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (!enc_) {
            LOGE("opus encoder: %s", opus_strerror(err));
            return;
        }
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_kbps * 1000));
        opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        net::tune_udp(sock_); // small and latency critical: voice queue, like input
        connect(sock_, reinterpret_cast<const sockaddr*>(&deck), sizeof(deck));
    }
    ~OpusSender()
    {
        if (enc_) opus_encoder_destroy(enc_);
        if (sock_ >= 0) close(sock_);
    }
    bool ok() const { return enc_ && sock_ >= 0; }

    void send(const int16_t* pcm, int frames, uint64_t capture_us)
    {
        uint8_t packet[sizeof(proto::AudioPacketHeader) + proto::kMaxAudioPayload];
        const int n = opus_encode(enc_, pcm, frames, packet + sizeof(proto::AudioPacketHeader),
                                  proto::kMaxAudioPayload);
        if (n <= 0) {
            LOGW("opus encode failed: %s", opus_strerror(n));
            return;
        }
        proto::AudioPacketHeader hdr{};
        hdr.magic = proto::kMagic;
        hdr.seq = seq_++;
        hdr.capture_us = capture_us;
        hdr.stream = uint8_t(stream_);
        hdr.samples = uint16_t(frames);
        std::memcpy(packet, &hdr, sizeof(hdr));
        ::send(sock_, packet, sizeof(hdr) + size_t(n), MSG_DONTWAIT | MSG_NOSIGNAL);
        ++packets;
        bytes += size_t(n);
    }

    uint64_t packets = 0, bytes = 0;

private:
    proto::AudioStream stream_;
    OpusEncoder* enc_ = nullptr;
    int sock_ = -1;
    uint32_t seq_ = 0;
};

int frame_samples(const AudioOptions& o)
{
    const int ms = o.frame_ms == 10 || o.frame_ms == 20 ? o.frame_ms : 5;
    return proto::kAudioRate / 1000 * ms;
}

// Desktop audio: record the default output's monitor and send it on.
void desktop_loop(std::shared_ptr<AudioStreamer::Shared> sh)
{
    const int frames = frame_samples(sh->options);
    const size_t bytes = size_t(frames) * proto::kAudioChannels * sizeof(int16_t);
    const pa_sample_spec spec{PA_SAMPLE_S16LE, uint32_t(proto::kAudioRate),
                              uint8_t(proto::kAudioChannels)};
    pa_buffer_attr attr;
    attr.maxlength = uint32_t(-1);
    attr.tlength = attr.prebuf = attr.minreq = uint32_t(-1);
    attr.fragsize = uint32_t(bytes); // deliver every frame as soon as it exists
    const std::string& src = sh->options.source;
    const char* device = src.empty() ? "@DEFAULT_MONITOR@" : src.c_str();
    int err = 0;
    pa_simple* pa = pa_simple_new(nullptr, "SteamU", PA_STREAM_RECORD, device,
                                  "Steam Deck audio", &spec, nullptr, &attr, &err);
    OpusSender sender(proto::AudioStream::Desktop, sh->deck, sh->options.bitrate_kbps);
    if (!pa) LOGE("cannot capture desktop audio from %s: %s", device, pa_strerror(err));
    else if (sender.ok()) {
        LOGI("desktop audio: capturing %s, Opus %d kbit/s, %d ms frames", device,
             sh->options.bitrate_kbps, frames * 1000 / proto::kAudioRate);
        std::vector<int16_t> pcm(size_t(frames) * proto::kAudioChannels);
        int silent_frames = 0;
        uint64_t stat_start = net::now_us(), last_packets = 0;
        while (!sh->stop) {
            if (pa_simple_read(pa, pcm.data(), bytes, &err) < 0) {
                LOGE("desktop audio capture failed: %s", pa_strerror(err));
                break;
            }
            const uint64_t now = net::now_us();
            // Never let capture latency build up (e.g. after a stall).
            const pa_usec_t lag = pa_simple_get_latency(pa, &err);
            if (lag != pa_usec_t(-1) && lag > 60000) {
                LOGD("desktop audio %llu us behind, flushing", (unsigned long long)lag);
                pa_simple_flush(pa, &err);
            }
            // Digital silence: after 100 ms, stop sending (saves Wi-Fi airtime;
            // the Deck re-buffers when sound starts again).
            bool silent = true;
            for (int16_t v : pcm)
                if (v) {
                    silent = false;
                    break;
                }
            silent_frames = silent ? silent_frames + frames : 0;
            const bool muted = sh->screens && sh->screens->desktop_audio_muted();
            if (!muted && silent_frames < proto::kAudioRate / 10)
                sender.send(pcm.data(), frames, now - uint64_t(frames) * 1000000 / proto::kAudioRate);

            if (now - stat_start >= 5000000) {
                LOGD("desktop audio: %llu packets in 5 s%s",
                     (unsigned long long)(sender.packets - last_packets), muted ? " (muted by app)" : "");
                last_packets = sender.packets;
                stat_start = now;
            }
        }
    }
    if (pa) pa_simple_free(pa);
    std::lock_guard lock(sh->mutex);
    sh->desktop_done = true;
    sh->cv.notify_all();
}

// App audio: PCM from the second-screen client, encoded as it arrives.
void app_loop(std::shared_ptr<AudioStreamer::Shared> sh)
{
    const int frames = frame_samples(sh->options);
    OpusSender sender(proto::AudioStream::App, sh->deck, sh->options.bitrate_kbps);
    if (!sender.ok()) return;
    std::vector<int16_t> pcm(size_t(frames) * proto::kAudioChannels);
    bool active = false;
    while (!sh->stop) {
        if (!sh->screens->take_audio(pcm.data(), size_t(frames), 100)) {
            if (active) LOGD("app audio idle");
            active = false;
            continue;
        }
        if (!active) LOGI("app audio: streaming to the Deck");
        active = true;
        sender.send(pcm.data(), frames, net::now_us());
    }
}

} // namespace

AudioStreamer::~AudioStreamer()
{
    stop();
}

void AudioStreamer::start(const AudioOptions& options, const sockaddr_in& deck,
                          ScreenServer* screens)
{
    stop();
    shared_ = std::make_shared<Shared>();
    shared_->options = options;
    shared_->deck = deck;
    shared_->screens = screens;
    LOGI("streaming audio to %s", net::to_string(deck).c_str());
    if (options.desktop) std::thread(desktop_loop, shared_).detach();
    else shared_->desktop_done = true;
    if (screens) app_thread_ = std::thread(app_loop, shared_);
}

void AudioStreamer::stop()
{
    if (!shared_) return;
    shared_->stop = true;
    if (app_thread_.joinable()) app_thread_.join();
    // The capture thread blocks in pa_simple_read until the next frame
    // (a few ms). Wait briefly; if the source stalled it finishes on its
    // own later and only touches the shared state it holds.
    std::unique_lock lock(shared_->mutex);
    shared_->cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return shared_->desktop_done; });
    lock.unlock();
    shared_.reset();
}

} // namespace steamu::host
