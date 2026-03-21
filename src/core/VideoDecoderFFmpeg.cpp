#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

#include <cmath>
#include <algorithm>
#include <deque>

#include <QElapsedTimer>

namespace trace::core {

struct VideoDecoderFFmpeg::Impl {
#ifdef TRACE_WITH_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    const AVCodec* codecDef = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int streamIndex = -1;

    AVRational streamTimeBase{0, 1};
    AVRational fpsQ{24, 1};
    int64_t streamStartTs = 0;
    long long lastDecodedFrame = -1;

    struct CachedFrame {
        long long frame = -1;
        QImage image;
    };
    std::deque<CachedFrame> reverseCache;
    int reverseCacheCapacity = 12;
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
    impl_->reverseCache.clear();
#endif
    currentFrame_ = -1;
    metadata_ = {};
    perfStats_ = {};
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

    // Let FFmpeg use codec-appropriate multi-thread decode for high-res 4K clips.
    impl_->codec->thread_count = 0;
    impl_->codec->thread_type = FF_THREAD_FRAME;

    if (avcodec_open2(impl_->codec, impl_->codecDef, nullptr) < 0) {
        error = "FFmpeg: codec open failed";
        close();
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        error = "FFmpeg: frame/packet alloc failed";
        close();
        return false;
    }

    const int w = impl_->codec->width;
    const int h = impl_->codec->height;

    impl_->sws = sws_getContext(
        w, h, impl_->codec->pix_fmt,
        w, h, AV_PIX_FMT_BGRA,
        SWS_POINT, nullptr, nullptr, nullptr);

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
    frameIndex = std::max<long long>(0, frameIndex);

    auto pushReverseCache = [&](long long cachedFrame, const QImage& image) {
        if (cachedFrame < 0 || image.isNull()) return;

        if (!impl_->reverseCache.empty() && impl_->reverseCache.back().frame == cachedFrame) {
            impl_->reverseCache.back().image = image;
            return;
        }

        impl_->reverseCache.push_back({cachedFrame, image});
        while (static_cast<int>(impl_->reverseCache.size()) > impl_->reverseCacheCapacity) {
            impl_->reverseCache.pop_front();
        }
    };

    auto tryReverseCache = [&](long long wantedFrame) -> bool {
        ++perfStats_.reverseCacheLookups;
        for (auto it = impl_->reverseCache.rbegin(); it != impl_->reverseCache.rend(); ++it) {
            if (it->frame == wantedFrame) {
                ++perfStats_.reverseCacheHits;
                outImage = it->image;
                currentFrame_ = wantedFrame;
                error.clear();
                return true;
            }
        }
        return false;
    };

    auto frameFromPts = [&](int64_t pts) -> long long {
        if (pts == AV_NOPTS_VALUE) return impl_->lastDecodedFrame + 1;
        const int64_t relPts = pts - impl_->streamStartTs;
        return av_rescale_q_rnd(relPts, impl_->streamTimeBase, frameTb, static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    };

    qint64 decodeNs = 0;
    qint64 convertNs = 0;

    auto updatePerfStats = [&]() {
        const double decodeMs = static_cast<double>(decodeNs) / 1'000'000.0;
        const double convertMs = static_cast<double>(convertNs) / 1'000'000.0;
        const double totalMs = decodeMs + convertMs;

        perfStats_.lastDecodeMs = decodeMs;
        perfStats_.lastConvertMs = convertMs;
        perfStats_.lastTotalMs = totalMs;

        ++perfStats_.samples;
        const double n = static_cast<double>(perfStats_.samples);
        perfStats_.avgDecodeMs += (decodeMs - perfStats_.avgDecodeMs) / n;
        perfStats_.avgConvertMs += (convertMs - perfStats_.avgConvertMs) / n;
        perfStats_.avgTotalMs += (totalMs - perfStats_.avgTotalMs) / n;
    };

    auto convertCurrentFrame = [&](QImage& image) {
        const int w = impl_->codec->width;
        const int h = impl_->codec->height;

        if (image.format() != QImage::Format_ARGB32 || image.width() != w || image.height() != h) {
            image = QImage(w, h, QImage::Format_ARGB32);
        }

        uint8_t* dstData[4] = { image.bits(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { image.bytesPerLine(), 0, 0, 0 };

        QElapsedTimer timer;
        timer.start();
        sws_scale(
            impl_->sws,
            impl_->frame->data,
            impl_->frame->linesize,
            0,
            h,
            dstData,
            dstLinesize);
        convertNs += timer.nsecsElapsed();
    };

    auto decodeUntilTarget = [&](long long target) -> bool {
        while (true) {
            QElapsedTimer readSendTimer;
            readSendTimer.start();
            const int readRes = av_read_frame(impl_->fmt, impl_->pkt);
            decodeNs += readSendTimer.nsecsElapsed();
            if (readRes < 0) break;

            if (impl_->pkt->stream_index != impl_->streamIndex) {
                av_packet_unref(impl_->pkt);
                continue;
            }

            QElapsedTimer sendTimer;
            sendTimer.start();
            const int sendRes = avcodec_send_packet(impl_->codec, impl_->pkt);
            decodeNs += sendTimer.nsecsElapsed();
            if (sendRes < 0) {
                av_packet_unref(impl_->pkt);
                continue;
            }
            av_packet_unref(impl_->pkt);

            while (true) {
                QElapsedTimer recvTimer;
                recvTimer.start();
                const int recvRes = avcodec_receive_frame(impl_->codec, impl_->frame);
                decodeNs += recvTimer.nsecsElapsed();
                if (recvRes != 0) break;
                const int64_t pts = (impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                        ? impl_->frame->best_effort_timestamp
                                        : impl_->frame->pts;
                long long decodedFrame = frameFromPts(pts);
                if (decodedFrame < 0) decodedFrame = 0;

                if (decodedFrame <= impl_->lastDecodedFrame) decodedFrame = impl_->lastDecodedFrame + 1;
                impl_->lastDecodedFrame = decodedFrame;

                if (decodedFrame < target) {
                    const long long deltaToTarget = target - decodedFrame;
                    if (deltaToTarget <= impl_->reverseCacheCapacity) {
                        QImage cached;
                        convertCurrentFrame(cached);
                        pushReverseCache(decodedFrame, cached);
                    }
                    continue;
                }

                convertCurrentFrame(outImage);
                currentFrame_ = decodedFrame;
                pushReverseCache(decodedFrame, outImage);
                return true;
            }
        }
        return false;
    };

    const bool requestIsBackward = currentFrame_ >= 0 && frameIndex < currentFrame_;
    if (requestIsBackward && tryReverseCache(frameIndex)) {
        updatePerfStats();
        return true;
    }

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

    updatePerfStats();
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
