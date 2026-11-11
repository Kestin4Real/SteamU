#include "host/obs_output.hpp"

#include "common/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace steamu::host {

namespace {

std::string av_error(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

} // namespace

ObsOutput::~ObsOutput()
{
    close();
}

bool ObsOutput::open(const std::string& url, const AVCodecContext* enc)
{
    close();
    url_ = url;
    avformat_network_init();

    int err = avformat_alloc_output_context2(&fmt_, nullptr, "mpegts", url.c_str());
    if (err < 0 || !fmt_) {
        LOGE("OBS output: cannot create MPEG-TS muxer: %s", av_error(err).c_str());
        fmt_ = nullptr;
        return false;
    }
    stream_ = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_from_context(stream_->codecpar, enc);
    stream_->time_base = {1, 90000};

    if ((err = avio_open(&fmt_->pb, url.c_str(), AVIO_FLAG_WRITE)) < 0) {
        LOGE("OBS output: cannot open %s: %s", url.c_str(), av_error(err).c_str());
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    // Low latency: no muxer-side buffering, repeat PAT/PMT so a receiver
    // can join at any time (e.g. when OBS starts after us).
    fmt_->flush_packets = 1;
    fmt_->max_delay = 0;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);
    av_dict_set(&opts, "mpegts_copyts", "1", 0);
    err = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        LOGE("OBS output: cannot start stream: %s", av_error(err).c_str());
        close();
        return false;
    }
    pkt_ = av_packet_alloc();
    base_us_ = 0;
    last_pts_ = -1;
    LOGI("OBS output: %s (add a Media Source with this URL, \"Local File\" unticked)",
         url.c_str());
    return true;
}

void ObsOutput::close()
{
    if (!fmt_) return;
    av_write_trailer(fmt_);
    if (fmt_->pb) avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    stream_ = nullptr;
    av_packet_free(&pkt_);
}

void ObsOutput::write(const AVPacket* pkt, uint64_t capture_us)
{
    if (!fmt_) return;
    if (!base_us_) base_us_ = capture_us - 1000000; // start at 1 s, never negative
    if (av_packet_ref(pkt_, pkt) < 0) return;
    int64_t pts = av_rescale_q(int64_t(capture_us - base_us_), AVRational{1, 1000000},
                               stream_->time_base);
    if (pts <= last_pts_) pts = last_pts_ + 1; // must strictly increase (keyframe resends)
    last_pts_ = pts;
    pkt_->pts = pkt_->dts = pts;
    pkt_->duration = 0;
    pkt_->stream_index = stream_->index;
    const int err = av_write_frame(fmt_, pkt_); // no interleaving delay: single stream
    av_packet_unref(pkt_);
    if (err < 0) {
        LOGW("OBS output: write failed (%s), disabling it", av_error(err).c_str());
        close();
    }
}

} // namespace steamu::host
