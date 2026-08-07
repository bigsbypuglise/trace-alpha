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

// Device buffer, in seconds. Set explicitly rather than left to the driver.
//
// The driver default is not stable across machines or Qt versions: the CI
// artifact (Qt 6.7.2) reported 192000 bytes and the local build (Qt 6.10.2)
// 96000, which on this device's float-stereo format (8 bytes/frame, 384000
// bytes/sec) is 500ms versus 250ms. Playback quality depends on that number,
// so it must be ours rather than inherited -- but note the buffer was *not*
// the root cause of the churn (see the scheduling comment in MainWindow's
// playback tick); it is the second-order term.
//
// Override with TRACE_AUDIO_BUFFER_MS to measure alternatives.
// Measured on the 1080p H.264 validation clip, single-clock scheduling, each
// row a full 241-frame run (no run dropped a frame -- the differences are
// holds, drift and A/V excursion):
//   500ms: 95.3% real time, 14 holds, drift -497ms, sync max 380ms
//   250ms: 97.6% real time,  8 holds, drift -251ms, sync max 130ms
//   100ms: 99.1% real time,  4 holds, drift  -87ms, sync max  62ms
// 100ms reaches the no-audio wall-clock control (98.7%), so there is nothing
// further to win by shrinking it, and a smaller device buffer only buys
// dropout risk on a loaded machine.
constexpr double kDeviceBufferSeconds = 0.10;

// Decoded audio in front of the device. MUST exceed the device buffer: the sink
// asks for its whole buffer on the first pull, and a ring smaller than that
// cannot answer, so readData padded the shortfall with silence -- which
// processedUSecs() then counted as elapsed media time, putting the clock ahead
// of the sound before a single real sample was heard. 2x is the measured-safe
// margin; the floor keeps short-buffer devices from thrashing the decode thread.
constexpr double kRingToDeviceRatio = 2.0;
constexpr double kMinRingSeconds = 0.5;

// Reported by AudioFeed::bytesAvailable so the sink never parks in IdleState.
// Any comfortably large value works; the sink clamps its reads to its own
// buffer size regardless.
constexpr qint64 kAlwaysAvailable = 1 << 20;

// How long start() will wait for the decode thread to prime the ring before
// handing the device its first pull. Bounded: a slow remote read must not
// freeze the play keypress. Anything not ready by then degrades to the old
// behaviour (silence padding) rather than blocking further.
constexpr int kMaxPrimeWaitMs = 150;

// Developer control test (Phase 1). With audio out of the picture entirely,
// video runs the wall-clock path, which isolates how much of the observed
// judder the audio master clock is responsible for.
bool audioDisabledByEnv() {
    static const bool disabled = qgetenv("TRACE_NO_AUDIO").toInt() != 0;
    return disabled;
}

// Freezing the device-latency term at its seeded value was measured against
// the sampled EMA and came out neutral (rep 9 skip 7 fixed vs rep 8 skip 6
// sampled, same rate, same drift), so the EMA stays: it is the documented,
// previously measured behaviour and there is no evidence for replacing it.
// The knob remains so the experiment can be repeated cheaply.
bool useFixedLatency() {
    static const bool fixed = qgetenv("TRACE_AUDIO_FIXED_LATENCY").toInt() != 0;
    return fixed;
}

// How hard the wall-clock projection is pulled toward the audio reading each
// update. A slower gain was the obvious suspect for the residual churn -- the
// loop chasing the processedUSecs() staircase -- but 0.004 (time constant ~10s)
// measured identical to 0.05 once the scheduling fix landed: 99.1% real time,
// 4 holds, 0 skips, drift -86 vs -87ms. So the documented value stays, and the
// knob exists to re-run that comparison rather than to be tuned blind.
double clockSlew() {
    static const double slew = [] {
        const QByteArray raw = qgetenv("TRACE_AUDIO_SLEW");
        if (!raw.isEmpty()) {
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (ok && v > 0.0 && v <= 1.0) return v;
        }
        return 0.05;
    }();
    return slew;
}

double configuredDeviceBufferSeconds() {
    static const double seconds = [] {
        const int ms = qgetenv("TRACE_AUDIO_BUFFER_MS").toInt();
        if (ms > 0) return static_cast<double>(ms) / 1000.0;
        return kDeviceBufferSeconds;
    }();
    return seconds;
}

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
    AudioFeed(AudioRing& ring,
              std::atomic<long long>& underruns,
              std::atomic<long long>& silenceBytes)
        : ring_(ring), underruns_(underruns), silenceBytes_(silenceBytes) {}

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
            //
            // Counted, because processedUSecs() cannot tell silence from sound:
            // every padded byte advances the audio clock without any audio
            // having been heard, so a large figure means the clock is running
            // ahead of the media. Measured with the ring invariant in place it
            // is 0 throughout playback and only accrues at end of stream, once
            // the audio track has run out and video is still going.
            std::fill(data + got, data + maxSize, 0);
            silenceBytes_ += (maxSize - got);
            ++underruns_;
        }
        return maxSize;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    AudioRing& ring_;
    std::atomic<long long>& underruns_;
    std::atomic<long long>& silenceBytes_;
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
    std::atomic<long long> silenceBytes{0};

    double startSeconds = 0.0;
    bool playing = false;
    bool muted = false;
    bool disabledByEnv = false;

    // Buffer geometry, resolved at start() against the sink the driver actually
    // gave us. Everything the clock loop is tuned against derives from these
    // rather than from a hard-coded nominal size.
    qint64 requestedBufferBytes = 0;
    qint64 actualBufferBytes = 0;
    int ringCapacityBytes = 0;
    double deviceBufferS = 0.0;
    double snapSeconds = 0.25;
    double startupFillMs = 0.0;

    // The device drains its buffer in chunks, so `bufferSize - bytesFree`
    // sampled at an arbitrary instant is a sawtooth spanning the whole buffer.
    // Subtracting it raw made the clock jitter by that much, and the tick
    // alternately held and skipped frames chasing it. Smooth the latency term
    // instead: true device latency is near-constant, the sampling noise is not.
    //
    // Seeded from the configured buffer duration, never from the first sample:
    // at start() the device is empty, so the first sample reads ~0 while the
    // true steady-state latency is nearly a whole buffer, and the loop would
    // open by chasing an offset we already know. Correctness, not a measured
    // win -- it is also what makes clockReady() a meaningful test.
    double smoothedLatencyS = -1.0;
    double lastClockS = 0.0;
    long long clockUpdates = 0;
    long long clockSnaps = 0;

    // processedUSecs() counts bytes handed to the device, so it advances in
    // whatever chunk the sink last pulled -- a staircase, not a ramp. Reading
    // media time straight off it made the playhead oscillate about a frame
    // either side of true, and the tick paid for that with ~1.2 held and ~1.2
    // skipped frames per second. So the clock runs on wall time and is
    // *disciplined* by audio rather than sampled from it: smooth between
    // pulls, and still locked to the sound card's rate over the long run.
    QElapsedTimer clockWall;
    double clockBase = 0.0;
    bool clockInit = false;

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
double AudioOutput::advanceClock() { return 0.0; }
double AudioOutput::peekClock() const { return 0.0; }
bool AudioOutput::clockReady() const { return false; }
long long AudioOutput::clockUpdateCount() const { return 0; }
bool AudioOutput::ended() const { return true; }
void AudioOutput::setMuted(bool) {}
bool AudioOutput::isMuted() const { return false; }
AudioPerfStats AudioOutput::stats() const { return {}; }

#else

bool AudioOutput::open(const QString& path, QString& error) {
    error.clear();
    close();
    impl_->path = path;

    // Control test: behave exactly as a picture-only file does, so video falls
    // back to the wall-clock path with nothing else changed.
    if (audioDisabledByEnv()) {
        impl_->disabledByEnv = true;
        return false;
    }

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
    impl_->feed = std::make_unique<AudioFeed>(impl_->ring, impl_->underruns,
                                              impl_->silenceBytes);
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
    impl_->silenceBytes = 0;
    impl_->disabledByEnv = false;
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

    // Ask for a known device buffer instead of accepting the driver's default.
    // The default was 1.0s on the Windows test box against constants tuned for
    // 0.5s, and that mismatch is what produced the startup hold/skip churn.
    const double bytesPerSecond = static_cast<double>(impl_->outRate)
                                * static_cast<double>(impl_->outBytesPerFrame);
    const double wantSeconds = configuredDeviceBufferSeconds();
    impl_->requestedBufferBytes = static_cast<qint64>(wantSeconds * bytesPerSecond);
    impl_->sink->setBufferSize(impl_->requestedBufferBytes);

    // The ring must be able to answer the sink's first pull in full, so size it
    // off what the sink will actually be, not off what we asked for -- drivers
    // are free to round or ignore the request.
    const double effectiveBufferS = impl_->bytesToSeconds(
        impl_->sink->bufferSize() > 0 ? impl_->sink->bufferSize()
                                      : impl_->requestedBufferBytes);
    const double ringSeconds = std::max(kMinRingSeconds,
                                        kRingToDeviceRatio * effectiveBufferS);
    const int capacity = static_cast<int>(ringSeconds * bytesPerSecond);

    impl_->ringCapacityBytes = capacity;
    impl_->ring.reset(capacity);
    impl_->stopRequested = false;
    impl_->decodeEnded = false;
    impl_->startSeconds = startSeconds;
    impl_->lastClockS = startSeconds;
    impl_->clockInit = false;
    impl_->clockBase = startSeconds;
    impl_->underruns = 0;
    impl_->silenceBytes = 0;
    impl_->clockUpdates = 0;
    impl_->clockSnaps = 0;

    impl_->thread = std::make_unique<Impl::DecodeThread>(impl_.get());
    impl_->thread->start();

    // Prime before the device is allowed its first pull. Starting the sink
    // against an empty ring lets readData pad the opening buffer with silence,
    // and processedUSecs() counts padding as elapsed media time -- so the clock
    // would begin life ahead of the sound. Measured cost here is ~10-13ms and
    // startup silence is 0.
    //
    // NOTE this is a blocking wait on the UI thread, bounded at
    // kMaxPrimeWaitMs. On local media it never approaches the cap; on a cold
    // high-latency source it could, which is the one interaction with the
    // responsive-I/O work worth re-checking.
    QElapsedTimer prime;
    prime.start();
    const int primeTarget = static_cast<int>(
        std::min<double>(capacity, effectiveBufferS * bytesPerSecond));
    while (impl_->ring.used() < primeTarget
           && prime.elapsed() < kMaxPrimeWaitMs
           && !impl_->decodeEnded) {
        QThread::msleep(1);
    }
    impl_->startupFillMs = static_cast<double>(prime.nsecsElapsed()) / 1e6;

    impl_->feed->open(QIODevice::ReadOnly);
    impl_->sink->setVolume(impl_->muted ? 0.0 : 1.0);
    impl_->sink->start(impl_->feed.get());
    impl_->playing = true;

    // Resolve the loop's tuning against the sink we actually got.
    impl_->actualBufferBytes = impl_->sink->bufferSize();
    impl_->deviceBufferS = impl_->bytesToSeconds(impl_->actualBufferBytes);

    // Seed the latency estimate at the steady state rather than at zero. The
    // device buffer fills over the first seconds of playback, so a
    // first-sample seed starts a full buffer wrong and the EMA cannot catch a
    // ramp that size -- which is what kept tripping the hard snap.
    impl_->smoothedLatencyS = impl_->deviceBufferS;

    // A snap marks a genuine discontinuity (a stall, an underrun run), not
    // routine buffer fill, so scale it to the buffer that actually exists
    // rather than leaving a fixed 0.25s that a large-buffer device could trip
    // on. Defensive: across every run measured here the snap fired zero times,
    // so this is insurance for devices unlike this one, not a fix.
    impl_->snapSeconds = std::max(0.25, 1.5 * impl_->deviceBufferS);
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

double AudioOutput::advanceClock() {
    if (!impl_ || !impl_->playing || !impl_->sink) return 0.0;
    ++impl_->clockUpdates;

    // processedUSecs counts everything handed to the device, including audio
    // sitting in its buffer that nobody has heard yet. Back that out, or the
    // clock runs a buffer ahead of the sound and video leads picture-to-track.
    const double processed = static_cast<double>(impl_->sink->processedUSecs()) / 1'000'000.0;
    const qint64 inFlight = impl_->sink->bufferSize() - impl_->sink->bytesFree();
    const double rawLatency = impl_->bytesToSeconds(std::max<qint64>(inFlight, 0));

    // Seeded in start() from the configured buffer duration; this branch only
    // covers a sink that somehow reported nothing back then. See
    // useFixedLatency() for why the EMA was kept.
    if (impl_->smoothedLatencyS < 0.0) {
        impl_->smoothedLatencyS = rawLatency;
    } else if (!useFixedLatency()) {
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

    // A large error is a real event (a stall, an underrun run), not sampling
    // noise: snap to it rather than crawling for seconds. Small errors are the
    // staircase, and get corrected gently. The threshold scales with the actual
    // device buffer -- a fixed 0.25s fired on every routine fill once the
    // driver handed us a 1.0s buffer, and each of those snaps was a visible
    // hold or skip.
    const double kSlew = clockSlew();
    if (std::abs(error) > impl_->snapSeconds) {
        impl_->clockBase = raw - wall;
        ++impl_->clockSnaps;
    } else {
        impl_->clockBase += kSlew * error;
    }

    // Media time must never run backwards: a frame is never presented twice in
    // the wrong order because the clock wobbled.
    impl_->lastClockS = std::max(impl_->lastClockS, impl_->clockBase + wall);
    return impl_->lastClockS;
}

bool AudioOutput::clockReady() const {
    if (!impl_ || !impl_->playing || !impl_->sink) return false;
    if (impl_->smoothedLatencyS < 0.0) return false;
    const double processed =
        static_cast<double>(impl_->sink->processedUSecs()) / 1'000'000.0;
    // Strictly greater: at exactly equal, raw is still clamped at startSeconds.
    return processed > impl_->smoothedLatencyS;
}

double AudioOutput::peekClock() const {
    if (!impl_ || !impl_->playing) return 0.0;
    return impl_->lastClockS;
}

long long AudioOutput::clockUpdateCount() const {
    return impl_ ? impl_->clockUpdates : 0;
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
    // peek, never advance: this is telemetry, and stepping the control loop
    // from here is what made playback timing depend on whether the HUD was on.
    s.clockSeconds = peekClock();
    s.bufferedMs = impl_->bytesToSeconds(impl_->ring.used()) * 1000.0;
    s.underruns = impl_->underruns;
    s.ended = ended();
    s.disabledByEnv = impl_->disabledByEnv;
    s.sinkBufferRequestedBytes = impl_->requestedBufferBytes;
    s.ringCapacityBytes = impl_->ringCapacityBytes;
    s.deviceBufferMs = impl_->bytesToSeconds(impl_->actualBufferBytes) * 1000.0;
    s.ringCapacityMs = impl_->bytesToSeconds(impl_->ringCapacityBytes) * 1000.0;
    s.startupFillMs = impl_->startupFillMs;
    s.silenceBytes = impl_->silenceBytes;
    s.clockUpdates = impl_->clockUpdates;
    s.smoothedLatencyMs = impl_->smoothedLatencyS * 1000.0;
    s.snapThresholdMs = impl_->snapSeconds * 1000.0;
    s.clockSnaps = impl_->clockSnaps;
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
