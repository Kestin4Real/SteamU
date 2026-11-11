#include "host/video_streamer.hpp"

#include "common/log.hpp"
#include "common/net.hpp"
#include "host/obs_output.hpp"
#include "host/screen_server.hpp"
#include "host/x11_capture.hpp"
#include "steamu_screen.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

namespace steamu::host {

namespace {

struct CodecCtxDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};

std::string av_error(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

void sleep_until_us(uint64_t t_us)
{
    timespec ts{};
    ts.tv_sec = time_t(t_us / 1000000);
    ts.tv_nsec = long(t_us % 1000000) * 1000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

std::unique_ptr<AVCodecContext, CodecCtxDeleter> open_encoder(proto::VideoCodec codec, int w, int h,
                                                              int fps, int bitrate_kbps)
{
    const char* name = codec == proto::VideoCodec::HEVC ? "hevc_nvenc" : "h264_nvenc";
    const AVCodec* enc = avcodec_find_encoder_by_name(name);
    if (!enc) {
        LOGE("encoder %s not available in this FFmpeg build", name);
        return nullptr;
    }
    std::unique_ptr<AVCodecContext, CodecCtxDeleter> ctx(avcodec_alloc_context3(enc));
    ctx->width = w;
    ctx->height = h;
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->time_base = {1, fps};
    ctx->framerate = {fps, 1};
    ctx->bit_rate = int64_t(bitrate_kbps) * 1000;
    ctx->rc_max_rate = ctx->bit_rate;
    // One frame of VBV: every frame (keyframes too) fits in ~1/fps s of link
    // time, which bounds the network delay of any single frame.
    ctx->rc_buffer_size = int(ctx->bit_rate / fps);
    ctx->gop_size = INT_MAX; // only send keyframes when the deck asks
    ctx->max_b_frames = 0;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;

    av_opt_set(ctx->priv_data, "preset", "p1", 0);
    av_opt_set(ctx->priv_data, "tune", "ull", 0);
    av_opt_set(ctx->priv_data, "rc", "cbr", 0);
    av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0);
    av_opt_set_int(ctx->priv_data, "delay", 0, 0);
    av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0);
    av_opt_set_int(ctx->priv_data, "no-scenecut", 1, 0);

    int err = avcodec_open2(ctx.get(), enc, nullptr);
    if (err < 0) {
        LOGE("cannot open %s: %s", name, av_error(err).c_str());
        return nullptr;
    }
    LOGI("encoder %s %dx%d@%d, %d kbit/s CBR", name, w, h, fps, bitrate_kbps);
    return ctx;
}

} // namespace

VideoStreamer::~VideoStreamer()
{
    stop();
}

void VideoStreamer::start(const VideoOptions& options, const sockaddr_in& deck, int width,
                          int height, int refresh_hz, ScreenServer* screens)
{
    stop();
    screens_ = screens;
    stop_ = false;
    keyframe_requested_ = true;
    const int fps = options.fps > 0 ? options.fps : std::clamp(refresh_hz, 30, 120);
    thread_ = std::thread([this, options, deck, width, height, fps] {
        run(options, deck, width, height, fps);
    });
}

void VideoStreamer::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

namespace {

std::unique_ptr<SwsContext, SwsDeleter> make_scaler(int sw, int sh, AVPixelFormat sf, int dw, int dh)
{
    std::unique_ptr<SwsContext, SwsDeleter> sws(sws_alloc_context());
    av_opt_set_int(sws.get(), "srcw", sw, 0);
    av_opt_set_int(sws.get(), "srch", sh, 0);
    av_opt_set_int(sws.get(), "src_format", sf, 0);
    av_opt_set_int(sws.get(), "dstw", dw, 0);
    av_opt_set_int(sws.get(), "dsth", dh, 0);
    av_opt_set_int(sws.get(), "dst_format", AV_PIX_FMT_NV12, 0);
    av_opt_set_int(sws.get(), "sws_flags", SWS_BILINEAR, 0);
    av_opt_set_int(sws.get(), "threads", 8, 0);
    if (sws_init_context(sws.get(), nullptr, nullptr) < 0) return nullptr;
    // RGB (full range) -> BT.709 limited range YUV.
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
    return sws;
}

} // namespace

void VideoStreamer::run(VideoOptions options, sockaddr_in deck, int width, int height, int fps)
{
    const proto::VideoCodec codec =
        options.codec == "h264" ? proto::VideoCodec::H264 : proto::VideoCodec::HEVC;
    auto enc = open_encoder(codec, width, height, fps, options.bitrate_kbps);
    if (!enc) return;

    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_NV12;
    frame->width = width;
    frame->height = height;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(frame.get(), 0) < 0) return;
    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    net::tune_udp_video(sock);
    int sndbuf = 4 << 20;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    connect(sock, reinterpret_cast<const sockaddr*>(&deck), sizeof(deck));
    LOGI("streaming video to %s", net::to_string(deck).c_str());

    // Optional copy for OBS. Recorders need regular keyframes to start and to
    // recover, so send one every 2 s while it is on (the Deck alone only
    // gets keyframes on request).
    ObsOutput obs;
    if (!options.obs_url.empty()) obs.open(options.obs_url, enc.get());
    constexpr uint64_t kObsKeyframeInterval = 2000000;
    uint64_t last_keyframe_us = 0;

    std::vector<uint8_t> datagram(sizeof(proto::VideoPacketHeader) + proto::kVideoChunkPayload);
    uint32_t frame_number = 0;
    int64_t pts = 0;
    bool have_picture = false; // `frame` holds a valid picture (for keyframe resends)

    // Stats
    uint64_t stat_start = net::now_us(), stat_frames = 0, stat_bytes = 0;
    uint64_t stat_grab = 0, stat_scale = 0, stat_encode = 0, stat_late = 0;

    // Encodes `frame` and sends it. capture_us = when the picture was made.
    auto encode_and_send = [&](uint64_t capture_us, uint64_t t_grab, uint64_t t_scale) -> bool {
        const uint64_t t2 = net::now_us();
        frame->pts = pts++;
        bool key = keyframe_requested_.exchange(false);
        if (obs.active() && t2 - last_keyframe_us >= kObsKeyframeInterval) key = true;
        if (key) last_keyframe_us = t2;
        frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (key) frame->flags |= AV_FRAME_FLAG_KEY;
        else frame->flags &= ~AV_FRAME_FLAG_KEY;
        int err = avcodec_send_frame(enc.get(), frame.get());
        if (err < 0) {
            LOGE("encode failed: %s", av_error(err).c_str());
            return false;
        }
        while ((err = avcodec_receive_packet(enc.get(), pkt.get())) == 0) {
            stat_grab += t_grab;
            stat_scale += t_scale;
            stat_encode += net::now_us() - t2;

            // Send in MTU-sized chunks
            const size_t size = size_t(pkt->size);
            proto::VideoPacketHeader hdr{};
            hdr.magic = proto::kMagic;
            hdr.frame = frame_number++;
            hdr.count = uint16_t((size + proto::kVideoChunkPayload - 1) / proto::kVideoChunkPayload);
            hdr.frame_size = uint32_t(size);
            hdr.capture_us = capture_us;
            hdr.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? proto::kVideoFlagKeyframe : 0;
            hdr.codec = uint8_t(codec);
            for (uint16_t i = 0; i < hdr.count; ++i) {
                const size_t off = size_t(i) * proto::kVideoChunkPayload;
                const size_t n = std::min(proto::kVideoChunkPayload, size - off);
                hdr.index = i;
                std::memcpy(datagram.data(), &hdr, sizeof(hdr));
                std::memcpy(datagram.data() + sizeof(hdr), pkt->data + off, n);
                send(sock, datagram.data(), sizeof(hdr) + n, MSG_NOSIGNAL);
            }
            obs.write(pkt.get(), capture_us);
            ++stat_frames;
            stat_bytes += size;
            av_packet_unref(pkt.get());
        }
        return true;
    };

    // --- Mirror source (X11), opened on first use ---------------------------
    X11Capture capture;
    bool capture_open = false;
    std::unique_ptr<SwsContext, SwsDeleter> mirror_sws;
    std::unique_ptr<AVFrame, FrameDeleter> mirror_src(av_frame_alloc());

    // --- App source (second-screen API) ------------------------------------
    std::unique_ptr<SwsContext, SwsDeleter> app_sws;
    uint64_t app_generation = UINT64_MAX;
    bool was_app = false;

    const uint64_t period = 1000000u / unsigned(fps);
    uint64_t next = net::now_us();
    while (!stop_) {
        const bool app = screens_ && screens_->active();
        if (app != was_app) {
            LOGI("video source: %s", app ? "second-screen app" : "monitor mirror");
            was_app = app;
            keyframe_requested_ = true; // clean switch
            next = net::now_us();
        }

        if (app) {
            // Event driven: encode as soon as the app submits a frame.
            ScreenServer::Frame f;
            if (!screens_->wait_frame(keyframe_requested_ ? 0 : 50, f)) {
                // No new frame. Re-encode the last one if a keyframe is due.
                if (keyframe_requested_ && have_picture && !encode_and_send(net::now_us(), 0, 0))
                    break;
                continue;
            }
            const uint64_t t0 = net::now_us();
            if (f.generation != app_generation) {
                app_sws = make_scaler(f.width, f.height,
                                      f.format == SUS_FORMAT_BGRA8 ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0,
                                      width, height);
                app_generation = f.generation;
            }
            bool ok = app_sws && av_frame_make_writable(frame.get()) >= 0;
            if (ok) {
                // Bottom-up images (glReadPixels): start at the last row and
                // walk upwards with a negative stride; no copy needed.
                const uint8_t* src = f.flip_y ? f.data + size_t(f.height - 1) * size_t(f.stride)
                                              : f.data;
                const int stride = f.flip_y ? -f.stride : f.stride;
                ok = sws_scale(app_sws.get(), &src, &stride, 0, f.height, frame->data,
                               frame->linesize) > 0;
            }
            screens_->release(f);
            if (!ok) continue;
            have_picture = true;
            if (!encode_and_send(t0, 0, net::now_us() - t0)) break;
        } else {
            sleep_until_us(next);
            next += period;
            const uint64_t t0 = net::now_us();
            if (t0 > next) { // fell behind: skip ahead instead of bursting
                next = t0 + period;
                ++stat_late;
            }
            if (!capture_open) {
                if (!capture.open(options.monitor, width, height)) break;
                mirror_sws = make_scaler(capture.width(), capture.height(), AV_PIX_FMT_BGR0, width,
                                         height);
                if (!mirror_sws) break;
                mirror_src->format = AV_PIX_FMT_BGR0;
                mirror_src->width = capture.width();
                mirror_src->height = capture.height();
                capture_open = true;
            }
            const uint8_t* pixels = capture.grab();
            if (!pixels) {
                LOGE("screen capture failed");
                break;
            }
            const uint64_t t1 = net::now_us();
            if (av_frame_make_writable(frame.get()) < 0) break;
            if (!mirror_src->buf[0]) {
                // Wrap the (fixed) shared-memory image without copying;
                // without a buffer, sws_scale_frame would duplicate it.
                mirror_src->buf[0] = av_buffer_create(
                    const_cast<uint8_t*>(pixels), size_t(capture.stride()) * size_t(capture.height()),
                    [](void*, uint8_t*) {}, nullptr, 0);
                mirror_src->data[0] = const_cast<uint8_t*>(pixels);
                mirror_src->linesize[0] = capture.stride();
            }
            const int sws_err = sws_scale_frame(mirror_sws.get(), frame.get(), mirror_src.get());
            if (sws_err < 0) {
                LOGE("scale failed: %s", av_error(sws_err).c_str());
                break;
            }
            have_picture = true;
            if (!encode_and_send(t0, t1 - t0, net::now_us() - t1)) break;
        }

        const uint64_t now = net::now_us();
        if (now - stat_start >= 5000000) {
            if (stat_frames) {
                const double secs = double(now - stat_start) / 1e6;
                LOGI("video %.0f fps, %.1f Mbit/s, grab %.1f ms, scale %.1f ms, encode %.1f ms%s",
                     double(stat_frames) / secs, double(stat_bytes) * 8 / secs / 1e6,
                     double(stat_grab) / double(stat_frames) / 1000,
                     double(stat_scale) / double(stat_frames) / 1000,
                     double(stat_encode) / double(stat_frames) / 1000,
                     stat_late ? ", falling behind!" : "");
            }
            stat_start = now;
            stat_frames = stat_bytes = stat_grab = stat_scale = stat_encode = stat_late = 0;
        }
    }
    close(sock);
    LOGI("video stream stopped");
}

} // namespace steamu::host
