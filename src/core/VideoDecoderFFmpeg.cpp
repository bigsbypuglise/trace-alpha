#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}
#endif

#include <cmath>
#include <algorithm>
#include <deque>

#include <QElapsedTimer>
#include <QByteArray>

namespace trace::core {

#ifdef TRACE_WITH_FFMPEG
namespace {

int swsFlagsFor(bool fast) {
    // Fast: cheapest conversion path for real-time playback/scrubbing.
    // Accurate: full chroma interpolation + accurate rounding for paused
    // frame inspection (Step mode), where exactness matters more than speed.
    return fast ? SWS_FAST_BILINEAR
                : (SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);
}

SwsContext* createSwsContext(int w, int h, AVPixelFormat srcFmt, AVPixelFormat dstFmt, bool fast) {
    const int flags = swsFlagsFor(fast);

#if LIBSWSCALE_VERSION_INT >= AV_VERSION_INT(6, 7, 100)
    // FFmpeg 5.1+: slice-threaded swscale. threads=0 means auto (per CPU count).
    SwsContext* ctx = sws_alloc_context();
    if (ctx) {
        av_opt_set_int(ctx, "srcw", w, 0);
        av_opt_set_int(ctx, "srch", h, 0);
        av_opt_set_int(ctx, "src_format", srcFmt, 0);
        av_opt_set_int(ctx, "dstw", w, 0);
        av_opt_set_int(ctx, "dsth", h, 0);
        av_opt_set_int(ctx, "dst_format", dstFmt, 0);
        av_opt_set_int(ctx, "sws_flags", flags, 0);
        av_opt_set_int(ctx, "threads", 0, 0);
        if (sws_init_context(ctx, nullptr, nullptr) < 0) {
            sws_freeContext(ctx);
            ctx = nullptr;
        }
        if (ctx) return ctx;
    }
#endif

    return sws_getContext(w, h, srcFmt, w, h, dstFmt, flags, nullptr, nullptr, nullptr);
}

bool envFlagSet(const char* name) {
    const QByteArray v = qgetenv(name);
    return !v.isEmpty() && v != "0" && v.compare("false", Qt::CaseInsensitive) != 0;
}

} // namespace
#endif

struct VideoDecoderFFmpeg::Impl {
#ifdef TRACE_WITH_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    const AVCodec* codecDef = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int streamIndex = -1;
    int swsSrcW = 0;
    int swsSrcH = 0;
    AVPixelFormat swsSrcPixFmt = AV_PIX_FMT_NONE;
    bool swsIsFast = false;
    AVPixelFormat dstPixFmt = AV_PIX_FMT_BGRA;

    // Env overrides for A/B testing; when neither is set, conversion quality
    // is chosen per request mode (fast for Playback/Scrub, accurate for Step).
    bool forceFastConvert = false;
    bool forceAccurateConvert = false;

    AVRational streamTimeBase{0, 1};
    AVRational fpsQ{24, 1};
    int64_t streamStartTs = 0;
    long long lastDecodedFrame = -1;
    int playbackDirection = 1;

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

void VideoDecoderFFmpeg::clearForwardQueue() {
    // The synchronous forward-fill queue was removed: it decoded several
    // frames per timer tick in bursts, causing a rhythmic stutter on 4K
    // ProRes. Playback now decodes exactly one frame per request.
    perfStats_.forwardQueueDepth = 0;
}

void VideoDecoderFFmpeg::setPlaybackDirection(int direction) {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;
    impl_->playbackDirection = direction < 0 ? -1 : 1;
#else
    Q_UNUSED(direction);
#endif
}

void VideoDecoderFFmpeg::setHandoffTiming(double handoffMs) {
    perfStats_.lastHandoffMs = handoffMs;
    const double sampleCount = perfStats_.samples > 0 ? static_cast<double>(perfStats_.samples) : 1.0;
    perfStats_.avgHandoffMs += (handoffMs - perfStats_.avgHandoffMs) / sampleCount;
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
    impl_->swsSrcW = 0;
    impl_->swsSrcH = 0;
    impl_->swsSrcPixFmt = AV_PIX_FMT_NONE;
    impl_->swsIsFast = false;
    impl_->streamTimeBase = {0, 1};
    impl_->fpsQ = {24, 1};
    impl_->streamStartTs = 0;
    impl_->lastDecodedFrame = -1;
    impl_->playbackDirection = 1;
    impl_->reverseCache.clear();
#endif
    currentFrame_ = -1;
    metadata_ = {};
    perfStats_ = {};
}

bool VideoDecoderFFmpeg::open(const QString& path, QString& error) {
#ifdef TRACE_WITH_FFMPEG
    close();

    QElapsedTimer openTimer;
    openTimer.start();

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

    impl_->codec->thread_count = 0;
    impl_->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

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

    impl_->forceFastConvert = envFlagSet("TRACE_PERF_FAST_CONVERT");
    impl_->forceAccurateConvert = envFlagSet("TRACE_PERF_ACCURATE_CONVERT");
    impl_->dstPixFmt = AV_PIX_FMT_BGRA;

    impl_->swsSrcW = w;
    impl_->swsSrcH = h;
    impl_->swsSrcPixFmt = impl_->codec->pix_fmt;
    impl_->swsIsFast = false;

    impl_->sws = createSwsContext(w, h, impl_->codec->pix_fmt, impl_->dstPixFmt, impl_->swsIsFast);
    if (!impl_->sws) {
        error = "FFmpeg: swscale init failed";
        close();
        return false;
    }

    metadata_.width = w;
    metadata_.height = h;
    metadata_.codecName = impl_->codecDef->name ? impl_->codecDef->name : "unknown";

    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(impl_->codec->pix_fmt);
    perfStats_.srcPixelFormat = srcDesc && srcDesc->name ? QString::fromUtf8(srcDesc->name) : QStringLiteral("unknown");
    perfStats_.srcBitDepth = srcDesc ? av_get_bits_per_pixel(srcDesc) : 0;
    perfStats_.dstPixelFormat = QStringLiteral("BGRX8888");
    perfStats_.experimentalFastPathEnabled = impl_->forceFastConvert;

    AVRational fr = av_guess_frame_rate(impl_->fmt, stream, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = AVRational{24, 1};
    impl_->fpsQ = fr;
    metadata_.fps = av_q2d(fr);

    impl_->streamTimeBase = stream->time_base;
    impl_->streamStartTs = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;

    if (stream->nb_frames > 0) metadata_.frameCount = static_cast<long long>(stream->nb_frames);
    else if (impl_->fmt->duration > 0) metadata_.frameCount = static_cast<long long>(std::floor((impl_->fmt->duration / static_cast<double>(AV_TIME_BASE)) * metadata_.fps));

    if (impl_->fmt->duration > 0) metadata_.durationSeconds = impl_->fmt->duration / static_cast<double>(AV_TIME_BASE);

    perfStats_.openMs = static_cast<double>(openTimer.nsecsElapsed()) / 1'000'000.0;
    perfStats_.forwardQueueCapacity = 0;

    error.clear();
    return true;
#else
    Q_UNUSED(path);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

bool VideoDecoderFFmpeg::decodeFrameAt(long long frameIndex, QImage& outImage, QString& error, RequestMode mode) {
#ifdef TRACE_WITH_FFMPEG
    if (!isOpen()) {
        error = "Video decoder not open";
        return false;
    }

    const AVRational frameTb{impl_->fpsQ.den, impl_->fpsQ.num};
    frameIndex = std::max<long long>(0, frameIndex);

    // Conversion quality per request: fast while frames are in motion
    // (playback, scrubbing), accurate when the user is inspecting a frame
    // (stepping / paused landing frame). Env vars override for A/B testing.
    bool wantFastConvert = (mode != RequestMode::Step);
    if (impl_->forceFastConvert) wantFastConvert = true;
    if (impl_->forceAccurateConvert) wantFastConvert = false;

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
    qint64 convertAllocNs = 0;
    qint64 convertWrapNs = 0;
    qint64 swsScaleNs = 0;
    qint64 memcpyNs = 0;

    auto updatePerfStats = [&]() {
        const double decodeMs = static_cast<double>(decodeNs) / 1'000'000.0;
        const double convertMs = static_cast<double>(convertNs) / 1'000'000.0;
        const double totalMs = decodeMs + convertMs;
        const double allocMs = static_cast<double>(convertAllocNs) / 1'000'000.0;
        const double wrapMs = static_cast<double>(convertWrapNs) / 1'000'000.0;
        const double swsMs = static_cast<double>(swsScaleNs) / 1'000'000.0;
        const double memcpyMs = static_cast<double>(memcpyNs) / 1'000'000.0;

        perfStats_.lastDecodeMs = decodeMs;
        perfStats_.lastConvertMs = convertMs;
        perfStats_.lastTotalMs = totalMs;
        perfStats_.lastConvertAllocMs = allocMs;
        perfStats_.lastConvertWrapMs = wrapMs;
        perfStats_.lastSwsScaleMs = swsMs;
        perfStats_.lastMemcpyMs = memcpyMs;

        ++perfStats_.samples;
        const double n = static_cast<double>(perfStats_.samples);
        perfStats_.avgDecodeMs += (decodeMs - perfStats_.avgDecodeMs) / n;
        perfStats_.avgConvertMs += (convertMs - perfStats_.avgConvertMs) / n;
        perfStats_.avgTotalMs += (totalMs - perfStats_.avgTotalMs) / n;
        perfStats_.avgConvertAllocMs += (allocMs - perfStats_.avgConvertAllocMs) / n;
        perfStats_.avgConvertWrapMs += (wrapMs - perfStats_.avgConvertWrapMs) / n;
        perfStats_.avgSwsScaleMs += (swsMs - perfStats_.avgSwsScaleMs) / n;
        perfStats_.avgMemcpyMs += (memcpyMs - perfStats_.avgMemcpyMs) / n;
    };

    auto convertCurrentFrame = [&](QImage& image) {
        const int w = impl_->frame->width > 0 ? impl_->frame->width : impl_->codec->width;
        const int h = impl_->frame->height > 0 ? impl_->frame->height : impl_->codec->height;
        const AVPixelFormat srcPixFmt = static_cast<AVPixelFormat>(impl_->frame->format);
        const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(srcPixFmt);
        perfStats_.srcPixelFormat = srcDesc && srcDesc->name ? QString::fromUtf8(srcDesc->name) : QStringLiteral("unknown");
        perfStats_.srcBitDepth = srcDesc ? av_get_bits_per_pixel(srcDesc) : 0;

        const bool needRebuild = !impl_->sws
            || impl_->swsSrcW != w
            || impl_->swsSrcH != h
            || impl_->swsSrcPixFmt != srcPixFmt
            || impl_->swsIsFast != wantFastConvert;

        if (needRebuild) {
            QElapsedTimer wrapTimer;
            wrapTimer.start();
            if (impl_->sws) sws_freeContext(impl_->sws);
            impl_->sws = createSwsContext(w, h, srcPixFmt, impl_->dstPixFmt, wantFastConvert);
            convertWrapNs += wrapTimer.nsecsElapsed();
            impl_->swsSrcW = w;
            impl_->swsSrcH = h;
            impl_->swsSrcPixFmt = srcPixFmt;
            impl_->swsIsFast = wantFastConvert;
        }
        perfStats_.swsContextReused = !needRebuild;
        perfStats_.experimentalFastPathEnabled = wantFastConvert;

        if (!impl_->sws) {
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        constexpr QImage::Format kFrameFormat = QImage::Format_BGRX8888;
#else
        constexpr QImage::Format kFrameFormat = QImage::Format_ARGB32;
#endif

        if (image.format() != kFrameFormat || image.width() != w || image.height() != h || image.isNull()) {
            QElapsedTimer allocTimer;
            allocTimer.start();
            image = QImage(w, h, kFrameFormat);
            convertAllocNs += allocTimer.nsecsElapsed();
        }

        if (image.isNull()) {
            return;
        }

        QElapsedTimer wrapTimer;
        wrapTimer.start();
        uint8_t* dstData[4] = { image.bits(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { image.bytesPerLine(), 0, 0, 0 };
        convertWrapNs += wrapTimer.nsecsElapsed();

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
        const qint64 swsNs = timer.nsecsElapsed();
        swsScaleNs += swsNs;
        convertNs += swsNs;

        perfStats_.fullFrameCopiesPerFrame = 1;
    };

    // Decode linearly until the target frame is produced. Exactly one output
    // frame is converted per request (plus near-target frames cached for
    // reverse stepping). No forward fill: steady per-tick cost avoids the
    // burst stutter the old queue caused on heavy codecs (4K ProRes).
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

                // decodedFrame >= target: convert and return it. Frames are
                // emitted in presentation order, so an overshoot means the
                // exact target does not exist in the stream; showing the
                // first frame at/after the target keeps ordering stable.
                convertCurrentFrame(outImage);
                currentFrame_ = decodedFrame;
                pushReverseCache(decodedFrame, outImage);
                return true;
            }
        }
        return !outImage.isNull();
    };

    const bool requestIsBackward = currentFrame_ >= 0 && frameIndex < currentFrame_;
    const bool requestIsSequentialForward = currentFrame_ >= 0 && frameIndex == currentFrame_ + 1;

    if (requestIsBackward && tryReverseCache(frameIndex)) {
        updatePerfStats();
        return true;
    }

    const bool needSeek =
        (currentFrame_ < 0) ||
        (frameIndex < currentFrame_) ||
        (mode == RequestMode::Scrub) ||
        (mode == RequestMode::Step && std::llabs(frameIndex - currentFrame_) > 1) ||
        (!requestIsSequentialForward && frameIndex > currentFrame_ + 1);

    if (needSeek) {
        const int64_t relTs = av_rescale_q(frameIndex, frameTb, impl_->streamTimeBase);
        const int64_t targetTs = impl_->streamStartTs + relTs;

        QElapsedTimer seekTimer;
        seekTimer.start();
        if (av_seek_frame(impl_->fmt, impl_->streamIndex, targetTs, AVSEEK_FLAG_BACKWARD) < 0) {
            error = "Seek failed";
            return false;
        }
        avcodec_flush_buffers(impl_->codec);
        const double seekMs = static_cast<double>(seekTimer.nsecsElapsed()) / 1'000'000.0;
        perfStats_.lastSeekMs = seekMs;
        ++perfStats_.seekSamples;
        const double seekN = static_cast<double>(perfStats_.seekSamples);
        perfStats_.avgSeekMs += (seekMs - perfStats_.avgSeekMs) / seekN;

        impl_->lastDecodedFrame = frameIndex > 0 ? frameIndex - 1 : -1;
    }

    if (!decodeUntilTarget(frameIndex)) {
        error = "No decodable frame at target position";
        return false;
    }

    if (perfStats_.firstFrameMs <= 0.0 && perfStats_.samples == 0) {
        perfStats_.firstFrameMs = static_cast<double>(decodeNs + convertNs) / 1'000'000.0;
    }

    updatePerfStats();
    error.clear();
    return true;
#else
    Q_UNUSED(frameIndex);
    Q_UNUSED(outImage);
    Q_UNUSED(mode);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

} // namespace trace::core
