#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#endif

#include <cmath>
#include <cstring>
#include <algorithm>

namespace trace::core {

struct VideoDecoderFFmpeg::Impl {
#ifdef TRACE_WITH_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    const AVCodec* codecDef = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgb = nullptr;
    AVPacket* pkt = nullptr;
    int streamIndex = -1;
    uint8_t* rgbBuffer = nullptr;

    AVRational streamTimeBase{0, 1};
    AVRational fpsQ{24, 1};
    int64_t streamStartTs = 0;
    long long lastDecodedFrame = -1;
#endif
};

VideoDecoderFFmpeg::VideoDecoderFFmpeg() : impl_(new Impl()) {}

VideoDecoderFFmpeg::~VideoDecoderFFmpeg() {
    close();
    delete impl_;
}

bool VideoDecoderFFmpeg::isOpen() const {
#ifdef TRACE_WITH_FFMPEG
    return impl_ && impl_->codec != nullptr;
#else
    return false;
#endif
}

void VideoDecoderFFmpeg::close() {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;

    if (impl_->rgbBuffer) av_free(impl_->rgbBuffer);
    impl_->rgbBuffer = nullptr;

    if (impl_->rgb) av_frame_free(&impl_->rgb);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->pkt) av_packet_free(&impl_->pkt);

    if (impl_->sws) sws_freeContext(impl_->sws);
    impl_->sws = nullptr;

    if (impl_->codec) avcodec_free_context(&impl_->codec);
    if (impl_->fmt) avformat_close_input(&impl_->fmt);

    impl_->streamIndex = -1;
    impl_->streamTimeBase = {0, 1};
    impl_->fpsQ = {24, 1};
    impl_->streamStartTs = 0;
    impl_->lastDecodedFrame = -1;
#endif
    currentFrame_ = -1;
    metadata_ = {};
}

bool VideoDecoderFFmpeg::open(const QString& path, QString& error) {
#ifdef TRACE_WITH_FFMPEG
    close();

    if (avformat_open_input(&impl_->fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        error = "FFmpeg: unable to open file";
        return false;
    }
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        error = "FFmpeg: unable to read stream info";
        close();
        return false;
    }

    impl_->streamIndex = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (impl_->streamIndex < 0) {
        error = "FFmpeg: no video stream";
        close();
        return false;
    }

    AVStream* stream = impl_->fmt->streams[impl_->streamIndex];
    const AVCodecParameters* par = stream->codecpar;

    impl_->codecDef = avcodec_find_decoder(par->codec_id);
    if (!impl_->codecDef) {
        error = "FFmpeg: unsupported codec";
        close();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(impl_->codecDef);
    if (!impl_->codec) {
        error = "FFmpeg: alloc codec context failed";
        close();
        return false;
    }

    if (avcodec_parameters_to_context(impl_->codec, par) < 0) {
        error = "FFmpeg: codec params init failed";
        close();
        return false;
    }

    if (avcodec_open2(impl_->codec, impl_->codecDef, nullptr) < 0) {
        error = "FFmpeg: codec open failed";
        close();
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->rgb = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->rgb || !impl_->pkt) {
        error = "FFmpeg: frame/packet alloc failed";
        close();
        return false;
    }

    const int w = impl_->codec->width;
    const int h = impl_->codec->height;
    const int rgbSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, w, h, 1);
    impl_->rgbBuffer = static_cast<uint8_t*>(av_malloc(rgbSize));
    if (!impl_->rgbBuffer) {
        error = "FFmpeg: rgb buffer alloc failed";
        close();
        return false;
    }

    av_image_fill_arrays(impl_->rgb->data, impl_->rgb->linesize, impl_->rgbBuffer, AV_PIX_FMT_RGB24, w, h, 1);

    impl_->sws = sws_getContext(
        w, h, impl_->codec->pix_fmt,
        w, h, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!impl_->sws) {
        error = "FFmpeg: swscale init failed";
        close();
        return false;
    }

    metadata_.width = w;
    metadata_.height = h;
    metadata_.codecName = impl_->codecDef->name ? impl_->codecDef->name : "unknown";

    AVRational fr = av_guess_frame_rate(impl_->fmt, stream, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = AVRational{24, 1};
    impl_->fpsQ = fr;
    metadata_.fps = av_q2d(fr);

    impl_->streamTimeBase = stream->time_base;
    impl_->streamStartTs = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;

    if (stream->nb_frames > 0) metadata_.frameCount = static_cast<long long>(stream->nb_frames);
    else if (impl_->fmt->duration > 0) metadata_.frameCount = static_cast<long long>(std::floor((impl_->fmt->duration / static_cast<double>(AV_TIME_BASE)) * metadata_.fps));

    if (impl_->fmt->duration > 0) metadata_.durationSeconds = impl_->fmt->duration / static_cast<double>(AV_TIME_BASE);

    error.clear();
    return true;
#else
    Q_UNUSED(path);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

bool VideoDecoderFFmpeg::decodeFrameAt(long long frameIndex, QImage& outImage, QString& error) {
#ifdef TRACE_WITH_FFMPEG
    if (!isOpen()) {
        error = "Video decoder not open";
        return false;
    }

    const AVRational frameTb{impl_->fpsQ.den, impl_->fpsQ.num};

    auto frameFromPts = [&](int64_t pts) -> long long {
        if (pts == AV_NOPTS_VALUE) return impl_->lastDecodedFrame + 1;
        const int64_t relPts = pts - impl_->streamStartTs;
        return av_rescale_q_rnd(relPts, impl_->streamTimeBase, frameTb, static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    };

    auto convertCurrentFrame = [&]() {
        sws_scale(
            impl_->sws,
            impl_->frame->data,
            impl_->frame->linesize,
            0,
            impl_->codec->height,
            impl_->rgb->data,
            impl_->rgb->linesize);

        QImage img(impl_->codec->width, impl_->codec->height, QImage::Format_RGB888);
        for (int y = 0; y < impl_->codec->height; ++y) {
            memcpy(img.scanLine(y), impl_->rgb->data[0] + y * impl_->rgb->linesize[0], static_cast<size_t>(impl_->codec->width * 3));
        }
        outImage = img;
    };

    auto decodeUntilTarget = [&](long long target) -> bool {
        while (av_read_frame(impl_->fmt, impl_->pkt) >= 0) {
            if (impl_->pkt->stream_index != impl_->streamIndex) {
                av_packet_unref(impl_->pkt);
                continue;
            }

            if (avcodec_send_packet(impl_->codec, impl_->pkt) < 0) {
                av_packet_unref(impl_->pkt);
                continue;
            }
            av_packet_unref(impl_->pkt);

            while (avcodec_receive_frame(impl_->codec, impl_->frame) == 0) {
                const int64_t pts = (impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                        ? impl_->frame->best_effort_timestamp
                                        : impl_->frame->pts;
                long long decodedFrame = frameFromPts(pts);
                if (decodedFrame < 0) decodedFrame = 0;

                // Maintain monotonic logical frame mapping during forward decode.
                if (decodedFrame <= impl_->lastDecodedFrame) decodedFrame = impl_->lastDecodedFrame + 1;
                impl_->lastDecodedFrame = decodedFrame;

                if (decodedFrame < target) continue;

                convertCurrentFrame();
                currentFrame_ = decodedFrame;
                return true;
            }
        }
        return false;
    };

    frameIndex = std::max<long long>(0, frameIndex);

    // Only seek when moving backward or when decoder has no prior state.
    // For forward playback, keep decode linear to preserve display order.
    const bool needSeek = (currentFrame_ < 0) || (frameIndex <= currentFrame_);
    if (needSeek) {
        const int64_t relTs = av_rescale_q(frameIndex, frameTb, impl_->streamTimeBase);
        const int64_t targetTs = impl_->streamStartTs + relTs;

        if (av_seek_frame(impl_->fmt, impl_->streamIndex, targetTs, AVSEEK_FLAG_BACKWARD) < 0) {
            error = "Seek failed";
            return false;
        }

        avcodec_flush_buffers(impl_->codec);
        impl_->lastDecodedFrame = frameIndex > 0 ? frameIndex - 1 : -1;
    }

    if (!decodeUntilTarget(frameIndex)) {
        error = "No decodable frame at target position";
        return false;
    }

    error.clear();
    return true;
#else
    Q_UNUSED(frameIndex);
    Q_UNUSED(outImage);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

} // namespace trace::core
