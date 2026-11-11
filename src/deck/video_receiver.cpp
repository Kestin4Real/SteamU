#include "deck/video_receiver.hpp"

#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace steamu::deck {

namespace {

AVPixelFormat pick_format(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_VAAPI && ctx->hw_device_ctx) return *f;
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
        if (*f != AV_PIX_FMT_VAAPI) return *f;
    return AV_PIX_FMT_NONE;
}

class Decoder {
public:
    ~Decoder() { close(); }

    bool open(proto::VideoCodec codec)
    {
        close();
        const AVCodec* dec = avcodec_find_decoder(codec == proto::VideoCodec::HEVC
                                                      ? AV_CODEC_ID_HEVC
                                                      : AV_CODEC_ID_H264);
        if (!dec) return false;
        ctx_ = avcodec_alloc_context3(dec);
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx_->get_format = pick_format;

        hw_ = av_hwdevice_ctx_create(&ctx_->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                                     nullptr, 0) == 0;
        if (!hw_) {
            // Software fallback (e.g. testing on an NVIDIA PC). Slice threads
            // only: frame threads would add a frame of delay per thread.
            ctx_->thread_count = 4;
            ctx_->thread_type = FF_THREAD_SLICE;
        }
        if (avcodec_open2(ctx_, dec, nullptr) < 0) {
            close();
            return false;
        }
        codec_ = codec;
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        LOGI("video decoder: %s, %s", dec->name, hw_ ? "VAAPI hardware" : "software");
        return true;
    }

    void close()
    {
        if (ctx_) avcodec_free_context(&ctx_);
        if (frame_) av_frame_free(&frame_);
        if (pkt_) av_packet_free(&pkt_);
        codec_ = proto::VideoCodec(0);
    }

    proto::VideoCodec codec() const { return codec_; }

    // data must have AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes after size.
    // Returns false on a decode error.
    bool decode(uint8_t* data, size_t size, int64_t pts, const VideoReceiver::FrameSink& sink)
    {
        pkt_->data = data;
        pkt_->size = int(size);
        pkt_->pts = pts;
        if (avcodec_send_packet(ctx_, pkt_) < 0) return false;
        for (;;) {
            int err = avcodec_receive_frame(ctx_, frame_);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return true;
            if (err < 0) return false;
            AVFrame* out = av_frame_alloc();
            if (frame_->format == AV_PIX_FMT_VAAPI) {
                // Download to NV12 for SDL (about 1 ms for 1280x800).
                if (av_hwframe_transfer_data(out, frame_, 0) < 0) {
                    av_frame_free(&out);
                    av_frame_unref(frame_);
                    return false;
                }
                out->pts = frame_->pts;
                out->color_range = frame_->color_range;
                out->colorspace = frame_->colorspace;
            } else {
                av_frame_move_ref(out, frame_);
            }
            av_frame_unref(frame_);
            sink(out);
        }
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool hw_ = false;
    proto::VideoCodec codec_{};
};

} // namespace

VideoReceiver::~VideoReceiver()
{
    stop();
}

uint16_t VideoReceiver::start(FrameSink sink, KeyframeRequest request_keyframe)
{
    stop();
    sink_ = std::move(sink);
    request_keyframe_ = std::move(request_keyframe);

    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock_ < 0) return 0;
    int rcvbuf = 8 << 20; // capped by net.core.rmem_max; keyframes come in bursts
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    net::tune_udp_video(sock_);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t len = sizeof(addr);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        close(sock_);
        sock_ = -1;
        return 0;
    }
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return ntohs(addr.sin_port);
}

void VideoReceiver::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
}

void VideoReceiver::run()
{
    Decoder decoder;

    // Frame being assembled.
    bool active = false;
    uint32_t frame_no = 0;
    proto::VideoPacketHeader info{};
    std::vector<uint8_t> data;
    std::vector<bool> have;
    uint32_t received = 0;

    bool have_last = false;
    uint32_t last_complete = 0;
    bool need_keyframe = true;
    uint64_t last_request = 0;

    uint64_t stat_start = net::now_us();
    uint64_t stat_frames = 0, stat_lost = 0, stat_decode_us = 0, stat_bytes = 0;

    auto ask_keyframe = [&] {
        need_keyframe = true;
        const uint64_t now = net::now_us();
        if (now - last_request > 100000) { // at most every 100 ms
            last_request = now;
            if (request_keyframe_) request_keyframe_();
        }
    };

    std::vector<uint8_t> buf(65536);
    while (!stop_) {
        pollfd p{sock_, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) {
            if (need_keyframe && have_last) ask_keyframe();
            continue;
        }
        const ssize_t n = recv(sock_, buf.data(), buf.size(), 0);
        if (n < ssize_t(sizeof(proto::VideoPacketHeader))) continue;
        proto::VideoPacketHeader hdr;
        std::memcpy(&hdr, buf.data(), sizeof(hdr));
        const size_t chunk = size_t(n) - sizeof(hdr);
        if (hdr.magic != proto::kMagic || hdr.count == 0 || hdr.index >= hdr.count ||
            hdr.frame_size > 16u << 20 ||
            size_t(hdr.count) * proto::kVideoChunkPayload < hdr.frame_size)
            continue;

        if (have_last && int32_t(hdr.frame - last_complete) <= 0) continue; // stale

        if (!active || hdr.frame != frame_no) {
            if (active && int32_t(hdr.frame - frame_no) < 0) continue; // older than current
            if (active) { // a newer frame started before this one completed
                ++stat_lost;
                ask_keyframe();
            }
            active = true;
            frame_no = hdr.frame;
            info = hdr;
            data.assign(hdr.frame_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            have.assign(hdr.count, false);
            received = 0;
        }
        if (have[hdr.index]) continue;
        const size_t off = size_t(hdr.index) * proto::kVideoChunkPayload;
        if (off + chunk > info.frame_size) continue;
        std::memcpy(data.data() + off, buf.data() + sizeof(hdr), chunk);
        have[hdr.index] = true;
        if (++received < info.count) continue;

        // Complete frame.
        active = false;
        if (have_last && frame_no != last_complete + 1) {
            stat_lost += frame_no - last_complete - 1;
            ask_keyframe();
        }
        have_last = true;
        last_complete = frame_no;

        const bool key = info.flags & proto::kVideoFlagKeyframe;
        if (need_keyframe && !key) continue; // would decode into garbage
        if (key) need_keyframe = false;

        if (decoder.codec() != proto::VideoCodec(info.codec)) {
            if (!decoder.open(proto::VideoCodec(info.codec))) {
                LOGE("cannot open decoder for codec %u", info.codec);
                continue;
            }
        }
        const uint64_t t0 = net::now_us();
        if (!decoder.decode(data.data(), info.frame_size, int64_t(info.capture_us), sink_)) {
            LOGW("decode error, requesting keyframe");
            ask_keyframe();
            continue;
        }
        stat_decode_us += net::now_us() - t0;
        stat_bytes += info.frame_size;
        ++stat_frames;

        const uint64_t now = net::now_us();
        if (now - stat_start >= 5000000) {
            const double secs = double(now - stat_start) / 1e6;
            LOGI("video in: %.0f fps, %.1f Mbit/s, decode %.1f ms, lost %llu",
                 double(stat_frames) / secs, double(stat_bytes) * 8 / secs / 1e6,
                 stat_frames ? double(stat_decode_us) / double(stat_frames) / 1000 : 0.0,
                 (unsigned long long)stat_lost);
            stat_start = now;
            stat_frames = stat_lost = stat_decode_us = stat_bytes = 0;
        }
    }
}

} // namespace steamu::deck
