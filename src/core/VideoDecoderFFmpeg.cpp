#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
}
#endif

#include <cmath>
#include <algorithm>
#include <deque>

#include <QElapsedTimer>
#include <QByteArray>

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
    int swsSrcW = 0;
    int swsSrcH = 0;
    AVPixelFormat swsSrcPixFmt = AV_PIX_FMT_NONE;
    AVPixelFormat dstPixFmt = AV_PIX_FMT_BGRA;
    bool experimentalFastPath = false;

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

    std::deque<CachedFrame> forwardQueue;
    int forwardQueueCapacity = 3;
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
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;
    impl_->forwardQueue.clear();
    perfStats_.forwardQueueDepth = 0;
#endif
}

void VideoDecoderFFmpeg::setPlaybackDirection(int direction) {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;
    const int normalized = direction < 0 ? -1 : 1;
    if (impl_->playbackDirection != normalized) {
        impl_->playbackDirection = normalized;
        clearForwardQueue();
    }
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
    impl_->streamTimeBase = {0, 1};
    impl_->fpsQ = {24, 1};
    impl_->streamStartTs = 0;
    impl_->lastDecodedFrame = -1;
    impl_->playbackDirection = 1;
    impl_->reverseCache.clear();
    impl_->forwardQueue.clear();
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

    const QByteArray perfMode = qgetenv("TRACE_PERF_FAST_CONVERT");
    impl_->experimentalFastPath = !perfMode.isEmpty() && perfMode != "0" && perfMode.compare("false", Qt::CaseInsensitive) != 0;
    impl_->dstPixFmt = AV_PIX_FMT_BGRA;

    impl_->swsSrcW = w;
    impl_->swsSrcH = h;
    impl_->swsSrcPixFmt = impl_->codec->pix_fmt;

    const int swsFlags = impl_->experimentalFastPath
        ? SWS_FAST_BILINEAR
        : (SWS_FAST_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);

    impl_->sws = sws_getCachedContext(
        nullptr,
        w, h, impl_->codec->pix_fmt,
        w, h, impl_->dstPixFmt,
        swsFlags,
        nullptr, nullptr, nullptr);

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
    perfStats_.dstPixelFormat = QStringLiteral("BGRA");
    perfStats_.experimentalFastPathEnabled = impl_->experimentalFastPath;

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
    perfStats_.forwardQueueCapacity = impl_->forwardQueueCapacity;

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

    auto updateForwardDepth = [&]() {
        perfStats_.forwardQueueDepth = static_cast<int>(impl_->forwardQueue.size());
        perfStats_.forwardQueueCapacity = impl_->forwardQueueCapacity;
    };

    auto pruneForwardQueue = [&](long long keepFromFrame) {
        while (!impl_->forwardQueue.empty() && impl_->forwardQueue.front().frame < keepFromFrame) {
            impl_->forwardQueue.pop_front();
        }
        updateForwardDepth();
    };

    auto pushForwardQueue = [&](long long cachedFrame, const QImage& image) {
        if (mode != RequestMode::Playback || impl_->playbackDirection < 0) return;
        if (cachedFrame < 0 || image.isNull()) return;

        if (!impl_->forwardQueue.empty() && impl_->forwardQueue.back().frame == cachedFrame) {
            impl_->forwardQueue.back().image = image;
            updateForwardDepth();
            return;
        }

        impl_->forwardQueue.push_back({cachedFrame, image});
        while (static_cast<int>(impl_->forwardQueue.size()) > impl_->forwardQueueCapacity) {
            impl_->forwardQueue.pop_front();
        }
        updateForwardDepth();
    };

    auto tryForwardQueue = [&](long long wantedFrame) -> bool {
        if (mode != RequestMode::Playback || impl_->playbackDirection < 0) return false;
        pruneForwardQueue(wantedFrame);
        for (auto it = impl_->forwardQueue.begin(); it != impl_->forwardQueue.end(); ++it) {
            if (it->frame == wantedFrame) {
                ++perfStats_.forwardQueueHits;
                outImage = it->image;
                currentFrame_ = wantedFrame;
                impl_->lastDecodedFrame = std::max(impl_->lastDecodedFrame, wantedFrame);
                impl_->forwardQueue.erase(it);
                updateForwardDepth();
                error.clear();
                return true;
            }
        }
        ++perfStats_.forwardQueueMisses;
        return false;
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
        updateForwardDepth();
    };

    auto convertCurrentFrame = [&](QImage& image) {
        const int w = impl_->frame->width > 0 ? impl_->frame->width : impl_->codec->width;
        const int h = impl_->frame->height > 0 ? impl_->frame->height : impl_->codec->height;
        const AVPixelFormat srcPixFmt = static_cast<AVPixelFormat>(impl_->frame->format);
        const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(srcPixFmt);
        perfStats_.srcPixelFormat = srcDesc && srcDesc->name ? QString::fromUtf8(srcDesc->name) : QStringLiteral("unknown");
        perfStats_.srcBitDepth = srcDesc ? av_get_bits_per_pixel(srcDesc) : 0;

        bool recreatedSws = false;
        if (!impl_->sws || impl_->swsSrcW != w || impl_->swsSrcH != h || impl_->swsSrcPixFmt != srcPixFmt) {
            recreatedSws = true;
            const int swsFlags = impl_->experimentalFastPath
                ? SWS_FAST_BILINEAR
                : (SWS_FAST_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);

            QElapsedTimer wrapTimer;
            wrapTimer.start();
            impl_->sws = sws_getCachedContext(
                impl_->sws,
                w, h, srcPixFmt,
                w, h, impl_->dstPixFmt,
                swsFlags,
                nullptr, nullptr, nullptr);
            convertWrapNs += wrapTimer.nsecsElapsed();
            impl_->swsSrcW = w;
            impl_->swsSrcH = h;
            impl_->swsSrcPixFmt = srcPixFmt;
        }
        perfStats_.swsContextReused = !recreatedSws;

        if (!impl_->sws) {
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        constexpr QImage::Format kDefaultFrameFormat = QImage::Format_BGRX8888;
#else
        constexpr QImage::Format kDefaultFrameFormat = QImage::Format_ARGB32;
#endif
        const QImage::Format kFrameFormat = impl_->experimentalFastPath ? QImage::Format_RGB32 : kDefaultFrameFormat;
        perfStats_.dstPixelFormat = impl_->experimentalFastPath ? QStringLiteral("RGB32") : QStringLiteral("BGRX8888/BGRA");

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

    auto decodeUntilTarget = [&](long long target, bool fillForwardQueue) -> bool {
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

                if (decodedFrame == target) {
                    convertCurrentFrame(outImage);
                    currentFrame_ = decodedFrame;
                    pushReverseCache(decodedFrame, outImage);
                    if (!fillForwardQueue) {
                        return true;
                    }
                    continue;
                }

                if (fillForwardQueue && decodedFrame > target) {
                    QImage queued;
                    convertCurrentFrame(queued);
                    pushForwardQueue(decodedFrame, queued);
                    if (static_cast<int>(impl_->forwardQueue.size()) >= impl_->forwardQueueCapacity) {
                        return !outImage.isNull();
                    }
                    continue;
                }

                if (decodedFrame > target && outImage.isNull()) {
                    convertCurrentFrame(outImage);
                    currentFrame_ = decodedFrame;
                    pushReverseCache(decodedFrame, outImage);
                    return true;
                }
            }
        }
        return !outImage.isNull();
    };

    const bool requestIsBackward = currentFrame_ >= 0 && frameIndex < currentFrame_;
    const bool requestIsSequentialForward = (mode == RequestMode::Playback && impl_->playbackDirection > 0 && currentFrame_ >= 0 && frameIndex == currentFrame_ + 1);

    if (requestIsSequentialForward && tryForwardQueue(frameIndex)) {
        updatePerfStats();
        return true;
    }

    if (requestIsBackward && tryReverseCache(frameIndex)) {
        clearForwardQueue();
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
        clearForwardQueue();

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

    const bool fillForwardQueue = (mode == RequestMode::Playback && impl_->playbackDirection > 0);
    if (!decodeUntilTarget(frameIndex, fillForwardQueue)) {
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
