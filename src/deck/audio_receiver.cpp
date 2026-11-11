#include "deck/audio_receiver.hpp"

#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}
#include <SDL3/SDL.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace steamu::deck {

namespace {

constexpr const char* kStreamNames[proto::kAudioStreams] = {"desktop", "app"};
constexpr double kBytesPerMs = proto::kAudioRate / 1000.0 * proto::kAudioChannels * sizeof(float);

struct Stream {
    const char* name = "";
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SDL_AudioStream* sdl = nullptr;
    std::vector<float> pcm; // interleaved
    bool have_seq = false;
    uint32_t last_seq = 0;
    bool playing = false;
    double avg_ms = 0;
    // stats
    uint64_t packets = 0, lost = 0, late = 0, rebuffers = 0, overflows = 0;
    double level_sum = 0;

    bool open(SDL_AudioDeviceID device, const SDL_AudioSpec& spec)
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) {
            LOGE("no Opus decoder in this build");
            return false;
        }
        dec = avcodec_alloc_context3(codec);
        dec->sample_rate = proto::kAudioRate;
        av_channel_layout_default(&dec->ch_layout, proto::kAudioChannels);
        if (avcodec_open2(dec, codec, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        sdl = SDL_CreateAudioStream(&spec, &spec);
        if (!sdl || !SDL_BindAudioStream(device, sdl)) {
            LOGE("cannot create audio stream: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void close()
    {
        if (sdl) SDL_DestroyAudioStream(sdl);
        if (dec) avcodec_free_context(&dec);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        sdl = nullptr;
    }

    double queued_ms() const { return double(SDL_GetAudioStreamQueued(sdl)) / kBytesPerMs; }

    void put_silence(double ms)
    {
        if (ms <= 0) return;
        std::vector<float> zero(size_t(ms * proto::kAudioRate / 1000) * proto::kAudioChannels, 0.f);
        SDL_PutAudioStreamData(sdl, zero.data(), int(zero.size() * sizeof(float)));
    }

    void handle(const proto::AudioPacketHeader& hdr, const uint8_t* data, size_t size, int target_ms)
    {
        if (have_seq) {
            const int32_t delta = int32_t(hdr.seq - last_seq);
            if (delta <= 0) {
                ++late;
                return;
            }
            if (delta > 1) lost += uint64_t(delta - 1);
        }
        have_seq = true;
        last_seq = hdr.seq;
        ++packets;

        // Decode (FFmpeg needs zeroed padding after the data).
        std::vector<uint8_t> buf(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(buf.data(), data, size);
        pkt->data = buf.data();
        pkt->size = int(size);
        if (avcodec_send_packet(dec, pkt) < 0) return;
        pcm.clear();
        while (avcodec_receive_frame(dec, frame) == 0) {
            const int n = frame->nb_samples, ch = std::min(frame->ch_layout.nb_channels, 2);
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * 2);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c) {
                    const int sc = std::min(c, ch - 1);
                    pcm[base + size_t(i) * 2 + size_t(c)] =
                        frame->format == AV_SAMPLE_FMT_FLTP
                            ? reinterpret_cast<const float*>(frame->extended_data[sc])[i]
                            : reinterpret_cast<const float*>(frame->data[0])[i * ch + sc];
                }
            av_frame_unref(frame);
        }
        if (pcm.empty()) return;
        const double packet_ms = double(pcm.size() / 2) * 1000.0 / proto::kAudioRate;

        // Buffer management (see header).
        double level = queued_ms();
        if (level < 0.5) {
            // Ran dry (start, PC went silent, or a late/lost burst): build the
            // cushion up again before this packet.
            if (playing) ++rebuffers;
            put_silence(target_ms - packet_ms);
            avg_ms = target_ms;
            playing = true;
        } else if (level > target_ms + 60) {
            // Far too much queued (e.g. a burst after a Wi-Fi stall): drop it
            // rather than lag behind the picture.
            ++overflows;
            SDL_ClearAudioStream(sdl);
            put_silence(target_ms - packet_ms);
            avg_ms = target_ms;
        }
        SDL_PutAudioStreamData(sdl, pcm.data(), int(pcm.size() * sizeof(float)));

        level = queued_ms();
        level_sum += level;
        avg_ms = avg_ms * 0.98 + level * 0.02;
        const float ratio = 1.f + float(std::clamp((avg_ms - target_ms) * 0.0003, -0.005, 0.005));
        SDL_SetAudioStreamFrequencyRatio(sdl, ratio);
    }
};

} // namespace

AudioReceiver::~AudioReceiver()
{
    stop();
}

uint16_t AudioReceiver::start(int buffer_ms)
{
    stop();
    buffer_ms_ = std::clamp(buffer_ms, 5, 500);
    const SDL_AudioSpec spec{SDL_AUDIO_F32, proto::kAudioChannels, proto::kAudioRate};
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device_) {
        LOGE("cannot open audio output: %s", SDL_GetError());
        return 0;
    }
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    net::tune_udp(sock_);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t len = sizeof(addr);
    if (sock_ < 0 || bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        LOGE("cannot open the audio socket");
        stop();
        return 0;
    }
    int frames = 0;
    SDL_AudioSpec dev_spec{};
    SDL_GetAudioDeviceFormat(device_, &dev_spec, &frames);
    LOGI("audio output: %s, %d Hz, %d-frame device buffer, %d ms jitter buffer",
         SDL_GetCurrentAudioDriver(), dev_spec.freq, frames, buffer_ms_);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return ntohs(addr.sin_port);
}

void AudioReceiver::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
    if (device_) SDL_CloseAudioDevice(device_);
    device_ = 0;
}

void AudioReceiver::run()
{
    const SDL_AudioSpec spec{SDL_AUDIO_F32, proto::kAudioChannels, proto::kAudioRate};
    Stream streams[proto::kAudioStreams];
    for (size_t i = 0; i < proto::kAudioStreams; ++i) {
        streams[i].name = kStreamNames[i];
        if (!streams[i].open(device_, spec)) {
            for (auto& s : streams) s.close();
            return;
        }
    }

    alignas(8) uint8_t buf[sizeof(proto::AudioPacketHeader) + proto::kMaxAudioPayload + 64];
    uint64_t stat_start = net::now_us();
    while (!stop_) {
        pollfd p{sock_, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            const ssize_t n = recv(sock_, buf, sizeof(buf), 0);
            proto::AudioPacketHeader hdr{};
            if (n > ssize_t(sizeof(hdr))) {
                std::memcpy(&hdr, buf, sizeof(hdr));
                if (hdr.magic == proto::kMagic && hdr.stream < proto::kAudioStreams)
                    streams[hdr.stream].handle(hdr, buf + sizeof(hdr), size_t(n) - sizeof(hdr),
                                               buffer_ms_);
            }
        }
        const uint64_t now = net::now_us();
        if (now - stat_start >= 5000000) {
            for (auto& s : streams) {
                if (!s.packets) continue;
                LOGI("audio %s: %.0f packets/s, lost %llu, late %llu, buffer %.1f ms (target %d), "
                     "rebuffers %llu, overflows %llu",
                     s.name, double(s.packets) / 5.0, (unsigned long long)s.lost,
                     (unsigned long long)s.late, s.level_sum / double(s.packets), buffer_ms_,
                     (unsigned long long)s.rebuffers, (unsigned long long)s.overflows);
                s.packets = s.lost = s.late = s.rebuffers = s.overflows = 0;
                s.level_sum = 0;
            }
            stat_start = now;
        }
    }
    for (auto& s : streams) s.close();
}

} // namespace steamu::deck
