#include "core/AudioOutput.h"

#ifdef TRACE_WITH_AUDIO

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57, 24, 100)
#error "Trace audio requires FFmpeg 5.1 or newer (AVChannelLayout API)."
#endif

#endif // TRACE_WITH_AUDIO

namespace trace::core {

#ifdef TRACE_WITH_AUDIO
namespace {

// Half a second of decoded audio in front of the device. Large enough that a
// 33ms video frame on the UI thread cannot starve playback, small enough that
// a seek does not have a long tail of stale sound to discard.
constexpr double kRingSeconds = 0.5;

// Reported by AudioFeed::bytesAvailable so the sink never parks in IdleState.
// Any comfortably large value works; the sink clamps its reads to its own
// buffer size regardless.
constexpr qint64 kAlwaysAvailable = 1 << 20;

AVSampleFormat avFormatFor(QAudioFormat::SampleFormat fmt) {
    switch (fmt) {
        case QAudioFormat::UInt8: return AV_SAMPLE_FMT_U8;
        case QAudioFormat::Int16: return AV_SAMPLE_FMT_S16;
        case QAudioFormat::Int32: return AV_SAMPLE_FMT_S32;
        case QAudioFormat::Float: return AV_SAMPLE_FMT_FLT;
        default: return AV_SAMPLE_FMT_NONE;
    }
}

// Byte ring shared between the decode thread (writer) and the device callback
// thread (reader). A mutex is ample here: the device wakes a few hundred times
// a second, not per sample.
class AudioRing {
public:
    void reset(int capacityBytes) {
        QMutexLocker lock(&mutex_);
        buffer_.assign(static_cast<size_t>(std::max(capacityBytes, 1)), 0);
        read_ = write_ = used_ = 0;
        stopped_ = false;
        notFull_.wakeAll();
    }

    void stop() {
        QMutexLocker lock(&mutex_);
        stopped_ = true;
        notFull_.wakeAll();
    }

    int used() const {
        QMutexLocker lock(&mutex_);
        return used_;
    }

    // Blocks until there is room or the ring is stopped. Returns false when
    // stopped, so the decode thread can unwind.
    bool write(const uint8_t* data, int bytes) {
        QMutexLocker lock(&mutex_);
        int written = 0;
        while (written < bytes) {
            while (used_ == static_cast<int>(buffer_.size()) && !stopped_) {
                notFull_.wait(&mutex_, 20);
            }
            if (stopped_) return false;
            const int chunk = std::min(bytes - written,
                                       static_cast<int>(buffer_.size()) - used_);
            for (int i = 0; i < chunk; ++i) {
                buffer_[static_cast<size_t>(write_)] = data[written + i];
                write_ = (write_ + 1) % static_cast<int>(buffer_.size());
            }
            used_ += chunk;
            written += chunk;
        }
        return true;
    }

    // Never blocks: the device callback must always be served. Short reads are
    // the caller's cue to emit silence.
    int read(uint8_t* out, int bytes) {
        QMutexLocker lock(&mutex_);
        const int chunk = std::min(bytes, used_);
        for (int i = 0; i < chunk; ++i) {
            out[i] = buffer_[static_cast<size_t>(read_)];
            read_ = (read_ + 1) % static_cast<int>(buffer_.size());
        }
        used_ -= chunk;
        if (chunk > 0) notFull_.wakeAll();
        return chunk;
    }

private:
    mutable QMutex mutex_;
    QWaitCondition notFull_;
    std::vector<uint8_t> buffer_;
    int read_ = 0;
    int write_ = 0;
    int used_ = 0;
    bool stopped_ = false;
};

// The QIODevice QAudioSink pulls from. Runs on Qt's audio thread.
class AudioFeed final : public QIODevice {
public:
    AudioFeed(AudioRing& ring, std::atomic<long long>& underruns)
        : ring_(ring), underruns_(underruns) {}

    bool isSequential() const override { return true; }

    // QAudioSink asks how much is readable before it reads, and parks itself in
    // IdleState if the answer is zero -- waiting on a readyRead() that a
    // custom device like this one never emits. The default implementation
    // returns 0 because it only counts QIODevice's own internal read buffer,
    // which we do not use. Leaving it unoverridden meant the sink started,
    // pulled nothing, kept processedUSecs() at zero, and froze the picture:
    // the audio clock never advanced off its start value.
    //
    // readData always satisfies the full request (silence on underrun), so
    // "data is always available" is the truthful answer here.
    qint64 bytesAvailable() const override {
        return kAlwaysAvailable + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (maxSize <= 0) return 0;
        const int got = ring_.read(reinterpret_cast<uint8_t*>(data),
                                   static_cast<int>(maxSize));
        if (got < maxSize) {
            // Hand the device silence rather than a short read: a short read
            // makes QAudioSink go idle and stop the clock. Silence keeps the
            // clock running, which keeps video moving through the gap.
            std::fill(data + got, data + maxSize, 0);
            ++underruns_;
        }
        return maxSize;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    AudioRing& ring_;
    std::atomic<long long>& underruns_;
};

} // namespace
#endif // TRACE_WITH_AUDIO

struct AudioOutput::Impl {
#ifdef TRACE_WITH_AUDIO
    // Decoding runs on its own thread so a 33ms 4K video frame on the UI thread
    // cannot underrun the device.
    class DecodeThread final : public QThread {
    public:
        explicit DecodeThread(Impl* owner) : owner_(owner) {}
        void run() override { owner_->decodeLoop(); }
    private:
        Impl* owner_;
    };

    QString path;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int streamIndex = -1;

    QAudioFormat deviceFormat;
    std::unique_ptr<QAudioSink> sink;
    std::unique_ptr<AudioFeed> feed;
    AudioRing ring;
    std::unique_ptr<DecodeThread> thread;

    // Written by the decode thread, read by the UI thread every HUD refresh.
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> decodeEnded{false};
    std::atomic<long long> underruns{0};

    double startSeconds = 0.0;
    bool playing = false;
    bool muted = false;

    // The device drains its buffer in chunks, so `bufferSize - bytesFree`
    // sampled at an arbitrary instant is a sawtooth spanning the whole buffer
    // (0.5s here). Subtracting it raw made the clock jitter by that much, and
    // the tick alternately held and skipped frames chasing it. Smooth the
    // latency term instead: true device latency is near-constant, the sampling
    // noise is not.
    mutable double smoothedLatencyS = -1.0;
    mutable double lastClockS = 0.0;

    // processedUSecs() counts bytes handed to the device, so it advances in
    // whatever chunk the sink last pulled -- a staircase, not a ramp. Reading
    // media time straight off it made the playhead oscillate about a frame
    // either side of true, and the tick paid for that with ~1.2 held and ~1.2
    // skipped frames per second. So the clock runs on wall time and is
    // *disciplined* by audio rather than sampled from it: smooth between
    // pulls, and still locked to the sound card's rate over the long run.
    mutable QElapsedTimer clockWall;
    mutable double clockBase = 0.0;
    mutable bool clockInit = false;

    QString codecName;
    int outRate = 0;
    int outChannels = 0;
    int outBytesPerFrame = 0;

    double bytesToSeconds(qint64 bytes) const {
        if (outBytesPerFrame <= 0 || outRate <= 0) return 0.0;
        return static_cast<double>(bytes)
             / (static_cast<double>(outBytesPerFrame) * static_cast<double>(outRate));
    }

    void decodeLoop() {
        std::vector<uint8_t> out;
        while (!stopRequested) {
            const int res = av_read_frame(fmt, pkt);
            if (res < 0) {
                // Drain whatever the codec is still holding, then finish.
                avcodec_send_packet(codec, nullptr);
                while (avcodec_receive_frame(codec, frame) >= 0) {
                    const bool ok = convertAndWrite(frame, out);
                    av_frame_unref(frame);
                    if (!ok) break;
                }
                decodeEnded = true;
                return;
            }
            if (pkt->stream_index != streamIndex) {
                av_packet_unref(pkt);
                continue;
            }
            const int sendRes = avcodec_send_packet(codec, pkt);
            av_packet_unref(pkt);
            if (sendRes < 0 && sendRes != AVERROR(EAGAIN)) continue;

            while (true) {
                const int recvRes = avcodec_receive_frame(codec, frame);
                if (recvRes == AVERROR(EAGAIN) || recvRes == AVERROR_EOF) break;
                if (recvRes < 0) break;
                const bool ok = convertAndWrite(frame, out);
                av_frame_unref(frame);
                if (!ok) return;  // ring stopped: we are shutting down
            }
        }
    }

    bool convertAndWrite(AVFrame* f, std::vector<uint8_t>& out) {
        if (!swr || f->nb_samples <= 0) return true;
        // Allow for resampler delay so a rate conversion never truncates.
        const int64_t delay = swr_get_delay(swr, codec->sample_rate);
        const int maxOut = static_cast<int>(
            av_rescale_rnd(delay + f->nb_samples, outRate, codec->sample_rate, AV_ROUND_UP));
        if (maxOut <= 0) return true;

        const size_t needed = static_cast<size_t>(maxOut) * static_cast<size_t>(outBytesPerFrame);
        if (out.size() < needed) out.resize(needed);

        uint8_t* dst[1] = { out.data() };
        const int converted = swr_convert(
            swr, dst, maxOut,
            const_cast<const uint8_t**>(f->data), f->nb_samples);
        if (converted <= 0) return true;

        return ring.write(out.data(), converted * outBytesPerFrame);
    }

    void teardownStream() {
        if (swr) { swr_free(&swr); swr = nullptr; }
        if (codec) { avcodec_free_context(&codec); codec = nullptr; }
        if (fmt) { avformat_close_input(&fmt); fmt = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        streamIndex = -1;
    }
#endif // TRACE_WITH_AUDIO
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    close();
}

#ifndef TRACE_WITH_AUDIO

bool AudioOutput::open(const QString&, QString& error) { error.clear(); return false; }
void AudioOutput::close() {}
bool AudioOutput::hasAudio() const { return false; }
void AudioOutput::start(double) {}
void AudioOutput::stop() {}
bool AudioOutput::isPlaying() const { return false; }
double AudioOutput::clockSeconds() const { return 0.0; }
bool AudioOutput::ended() const { return true; }
void AudioOutput::setMuted(bool) {}
bool AudioOutput::isMuted() const { return false; }
AudioPerfStats AudioOutput::stats() const { return {}; }

#else

bool AudioOutput::open(const QString& path, QString& error) {
    error.clear();
    close();
    impl_->path = path;

    if (avformat_open_input(&impl_->fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        impl_->fmt = nullptr;
        return false;  // the video path reports open failures; stay quiet here
    }
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        impl_->teardownStream();
        return false;
    }

    const AVCodec* decoder = nullptr;
    const int idx = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (idx < 0 || !decoder) {
        // No audio track. Not an error: plenty of renders are picture only.
        impl_->teardownStream();
        return false;
    }
    impl_->streamIndex = idx;

    impl_->codec = avcodec_alloc_context3(decoder);
    if (!impl_->codec) {
        impl_->teardownStream();
        error = "Audio: codec context allocation failed";
        return false;
    }
    if (avcodec_parameters_to_context(impl_->codec, impl_->fmt->streams[idx]->codecpar) < 0
        || avcodec_open2(impl_->codec, decoder, nullptr) < 0) {
        impl_->teardownStream();
        error = "Audio: decoder open failed";
        return false;
    }
    impl_->codecName = QString::fromUtf8(decoder->name);

    // Convert to whatever the default device actually wants rather than asking
    // it to accept the file's format: Windows shared-mode endpoints advertise
    // one mix format and refuse the rest.
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        impl_->teardownStream();
        error = "Audio: no output device";
        return false;
    }
    impl_->deviceFormat = device.preferredFormat();
    const AVSampleFormat outFmt = avFormatFor(impl_->deviceFormat.sampleFormat());
    if (outFmt == AV_SAMPLE_FMT_NONE) {
        impl_->teardownStream();
        error = "Audio: unsupported device sample format";
        return false;
    }

    impl_->outRate = impl_->deviceFormat.sampleRate();
    impl_->outChannels = impl_->deviceFormat.channelCount();
    impl_->outBytesPerFrame = impl_->deviceFormat.bytesPerFrame();
    if (impl_->outRate <= 0 || impl_->outChannels <= 0 || impl_->outBytesPerFrame <= 0) {
        impl_->teardownStream();
        error = "Audio: device reported an unusable format";
        return false;
    }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, impl_->outChannels);
    const int swrRes = swr_alloc_set_opts2(
        &impl_->swr,
        &outLayout, outFmt, impl_->outRate,
        &impl_->codec->ch_layout, impl_->codec->sample_fmt, impl_->codec->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (swrRes < 0 || !impl_->swr || swr_init(impl_->swr) < 0) {
        impl_->teardownStream();
        error = "Audio: resampler init failed";
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        impl_->teardownStream();
        error = "Audio: frame allocation failed";
        return false;
    }

    impl_->sink = std::make_unique<QAudioSink>(device, impl_->deviceFormat);
    impl_->feed = std::make_unique<AudioFeed>(impl_->ring, impl_->underruns);
    return true;
}

void AudioOutput::close() {
    if (!impl_) return;
    stop();
    impl_->sink.reset();
    impl_->feed.reset();
    impl_->teardownStream();
    impl_->codecName.clear();
    impl_->underruns = 0;
}

bool AudioOutput::hasAudio() const {
    return impl_ && impl_->streamIndex >= 0 && impl_->sink != nullptr;
}

void AudioOutput::start(double startSeconds) {
    if (!hasAudio()) return;
    stop();

    // The decode thread is joined, so the demuxer can be seeked directly.
    const AVRational tb = impl_->fmt->streams[impl_->streamIndex]->time_base;
    const int64_t ts = static_cast<int64_t>(
        startSeconds * static_cast<double>(tb.den) / static_cast<double>(tb.num));
    av_seek_frame(impl_->fmt, impl_->streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(impl_->codec);

    const int capacity = static_cast<int>(kRingSeconds
                                          * static_cast<double>(impl_->outRate)
                                          * static_cast<double>(impl_->outBytesPerFrame));
    impl_->ring.reset(capacity);
    impl_->stopRequested = false;
    impl_->decodeEnded = false;
    impl_->startSeconds = startSeconds;
    impl_->smoothedLatencyS = -1.0;
    impl_->lastClockS = startSeconds;
    impl_->clockInit = false;
    impl_->clockBase = startSeconds;

    impl_->thread = std::make_unique<Impl::DecodeThread>(impl_.get());
    impl_->thread->start();

    impl_->feed->open(QIODevice::ReadOnly);
    impl_->sink->setVolume(impl_->muted ? 0.0 : 1.0);
    impl_->sink->start(impl_->feed.get());
    impl_->playing = true;
}

void AudioOutput::stop() {
    if (!impl_) return;
    if (impl_->sink) impl_->sink->stop();
    if (impl_->feed && impl_->feed->isOpen()) impl_->feed->close();

    impl_->stopRequested = true;
    impl_->ring.stop();
    if (impl_->thread) {
        impl_->thread->wait();
        impl_->thread.reset();
    }
    impl_->playing = false;
}

bool AudioOutput::isPlaying() const {
    return impl_ && impl_->playing;
}

double AudioOutput::clockSeconds() const {
    if (!impl_ || !impl_->playing || !impl_->sink) return 0.0;
    // processedUSecs counts everything handed to the device, including audio
    // sitting in its buffer that nobody has heard yet. Back that out, or the
    // clock runs a buffer ahead of the sound and video leads picture-to-track.
    const double processed = static_cast<double>(impl_->sink->processedUSecs()) / 1'000'000.0;
    const qint64 inFlight = impl_->sink->bufferSize() - impl_->sink->bytesFree();
    const double rawLatency = impl_->bytesToSeconds(std::max<qint64>(inFlight, 0));

    if (impl_->smoothedLatencyS < 0.0) {
        impl_->smoothedLatencyS = rawLatency;
    } else {
        constexpr double kAlpha = 0.02;  // slow: real latency barely moves
        impl_->smoothedLatencyS += kAlpha * (rawLatency - impl_->smoothedLatencyS);
    }

    const double raw = impl_->startSeconds
                     + std::max(0.0, processed - impl_->smoothedLatencyS);

    if (!impl_->clockInit) {
        impl_->clockInit = true;
        impl_->clockWall.start();
        impl_->clockBase = raw;
        impl_->lastClockS = raw;
        return raw;
    }

    const double wall = static_cast<double>(impl_->clockWall.nsecsElapsed()) / 1e9;
    const double error = raw - (impl_->clockBase + wall);

    // A large error is a real event (startup fill, a stall, an underrun run),
    // not sampling noise: snap to it rather than crawling for seconds. Small
    // errors are the staircase, and get corrected gently.
    constexpr double kSnapSeconds = 0.25;
    constexpr double kSlew = 0.05;
    if (std::abs(error) > kSnapSeconds) {
        impl_->clockBase = raw - wall;
    } else {
        impl_->clockBase += kSlew * error;
    }

    // Media time must never run backwards: a frame is never presented twice in
    // the wrong order because the clock wobbled.
    impl_->lastClockS = std::max(impl_->lastClockS, impl_->clockBase + wall);
    return impl_->lastClockS;
}

bool AudioOutput::ended() const {
    if (!impl_) return true;
    return impl_->decodeEnded && impl_->ring.used() == 0;
}

void AudioOutput::setMuted(bool muted) {
    if (!impl_) return;
    impl_->muted = muted;
    if (impl_->sink) impl_->sink->setVolume(muted ? 0.0 : 1.0);
}

bool AudioOutput::isMuted() const {
    return impl_ && impl_->muted;
}

AudioPerfStats AudioOutput::stats() const {
    AudioPerfStats s;
    if (!impl_) return s;
    s.available = hasAudio();
    s.playing = impl_->playing;
    s.muted = impl_->muted;
    s.codecName = impl_->codecName;
    s.sampleRate = impl_->outRate;
    s.channels = impl_->outChannels;
    s.clockSeconds = clockSeconds();
    s.bufferedMs = impl_->bytesToSeconds(impl_->ring.used()) * 1000.0;
    s.underruns = impl_->underruns;
    s.ended = ended();
    if (impl_->sink) {
        s.processedUSecs = impl_->sink->processedUSecs();
        s.sinkBufferBytes = impl_->sink->bufferSize();
        s.sinkFreeBytes = impl_->sink->bytesFree();
        s.sinkState = static_cast<int>(impl_->sink->state());
    }
    return s;
}

#endif // TRACE_WITH_AUDIO

} // namespace trace::core
