#pragma once

#include <QString>
#include <QSize>
#include <functional>
#include "core/MediaIoSource.h"
#include "core/VideoFrame.h"

namespace trace::core {

struct VideoMetadata {
    int width = 0;
    int height = 0;
    double fps = 24.0;
    // The same rate as the container states it, kept exact. `fps` is av_q2d of
    // this, and 24000/1001 has no exact double -- so every consumer working
    // from the double (tick interval, timecode, seek arithmetic) inherits an
    // approximation of a number the file states precisely. Presentation timing
    // cannot be measured honestly against a rate that is already rounded, so
    // the rational is retained here rather than reconstructed downstream.
    //
    // Deliberately int/int rather than AVRational: this header is reached from
    // MainWindow.h and must compile with TRACE_WITH_FFMPEG undefined, so no
    // FFmpeg type may appear in it.
    int fpsNum = 24;
    int fpsDen = 1;
    long long frameCount = -1;
    double durationSeconds = 0.0;
    QString codecName;
    // Every frame is a keyframe (ProRes, DNxHD, MJPEG). Already used to pick
    // the threading mode; exposed because it is also the exact answer to "does
    // a seek land on the target, or does it land before it and walk?", which
    // decides whether a scrub preview may skip frames.
    bool intraOnly = false;
};

struct VideoPerfStats {
    double lastDecodeMs = 0.0;
    double lastConvertMs = 0.0;
    double lastTotalMs = 0.0;
    double avgDecodeMs = 0.0;
    double avgConvertMs = 0.0;
    double avgTotalMs = 0.0;
    double openMs = 0.0;
    double firstFrameMs = 0.0;
    double lastSeekMs = 0.0;
    double avgSeekMs = 0.0;

    double lastConvertAllocMs = 0.0;
    double avgConvertAllocMs = 0.0;
    double lastConvertWrapMs = 0.0;
    double avgConvertWrapMs = 0.0;
    // "wrap" is an aggregate bucket; these split it into its real stages so a
    // cost showing up there can be attributed instead of guessed at.
    double lastCtxRebuildMs = 0.0;
    double avgCtxRebuildMs = 0.0;
    double lastDetachMs = 0.0;
    double avgDetachMs = 0.0;
    long long lastCtxRebuilds = 0;
    // Active-drag preview. When the scrub walk cap engages the displayed frame
    // trails the requested one; release resolves the exact frame and this
    // reports false again.
    bool previewApproximate = false;
    long long previewTargetFrame = -1;
    long long previewDisplayedFrame = -1;

    // Conversion-context set. After warm-up rebuilds should stop climbing even
    // as scrub preview and exact output alternate.
    long long swsSlotRebuilds = 0;
    int swsSlotsInUse = 0;
    long long lastConvertCalls = 0;
    bool lastImageWasShared = false;
    double lastSwsScaleMs = 0.0;
    double avgSwsScaleMs = 0.0;
    double lastMemcpyMs = 0.0;
    double avgMemcpyMs = 0.0;
    double lastHandoffMs = 0.0;
    double avgHandoffMs = 0.0;

    // Seek-walk cost: on long-GOP codecs a frame-exact seek lands on a
    // keyframe and decodes forward to the target. These expose how much of
    // the landing delay is the decode walk vs. speculative cache conversion.
    long long lastWalkFrames = 0;
    long long lastWalkCacheConverts = 0;
    double lastWalkCacheConvertMs = 0.0;

    // End-of-stream drain accounting.
    long long drainPacketsSent = 0;
    long long drainFramesRecovered = 0;
    long long staleSuccessPrevented = 0;
    // Walks given up because the caller's target moved while they were running.
    // Distinct from a failure: the frame was not produced because it is no
    // longer wanted, not because it could not be decoded. Must never take the
    // recovery-seek path -- `recov` is a correctness counter and an abandoned
    // walk is not a mispositioned decoder.
    long long walksAbandoned = 0;
    // Longest stretch a walk went between consecutive chances to notice a
    // cancellation. This is the decoder's contribution to cancellation
    // latency; the worker measures the rest.
    double maxCheckpointGapMs = 0.0;
    // Decodes that failed with the decoder mispositioned and succeeded after a
    // recovery seek. Should stay 0: anything above it means some path is still
    // moving currentFrame_ without moving the decoder.
    long long recoveredDecodeFailures = 0;

    // Frame cache accounting.
    // cacheCapacity is how many entries of the size currently being stored fit
    // the byte budget, so it moves as a drag fills the cache with half-res
    // previews rather than full-res frames. Eviction is by cacheBytes against
    // cacheBudgetBytes; the count is reported, not enforced.
    int cacheCapacity = 0;
    int cacheOccupancy = 0;
    long long cacheInserts = 0;
    long long cacheEvictions = 0;
    long long cacheBytes = 0;
    long long cacheBudgetBytes = 0;

    long long seekSamples = 0;
    long long samples = 0;
    long long reverseCacheHits = 0;
    long long reverseCacheLookups = 0;
    long long forwardQueueHits = 0;
    long long forwardQueueMisses = 0;
    int forwardQueueDepth = 0;
    int forwardQueueCapacity = 0;
    long long lateFrames = 0;

    // Storage/IO telemetry. Populated from MediaIoSource; see ioStats().
    QString sourceStorage;       // "local" / "remote", with the deciding reason
    QString sourceVolume;
    qint64 sourceBytes = 0;
    double sourceBitrateMbps = 0.0;
    qint64 sourceReadPos = 0;
    int ioBufferBytes = 0;
    // Open-path breakdown. On a virtual mount the time to open a file is often
    // not in reads at all, so these are tracked separately or the cost cannot
    // be attributed.
    double openClassifyMs = 0.0;
    bool classifyCached = false;
    double openFileMs = 0.0;
    double openDemuxMs = 0.0;
    double openStreamInfoMs = 0.0;
    // What avformat_find_stream_info actually cost in I/O, and the limits it
    // ran under. Probing is the dominant term in open time on every source
    // measured, so it is broken out rather than inferred.
    qint64 probeSizeLimit = 0;
    qint64 analyzeDurationUs = 0;
    long long probeReads = 0;
    long long probeBytes = 0;
    long long probeSeeks = 0;
    int streamCount = 0;

    QString srcPixelFormat;
    QString dstPixelFormat;
    // YUV->RGB matrix and range actually used for conversion, so a wrong-looking
    // image can be checked against what the file claims rather than guessed at.
    QString colorMatrix;
    bool srcFullRange = false;
    bool colorMatrixInferred = false;
    int srcBitDepth = 0;
    bool swsContextReused = false;
    bool alphaPlaneSkipped = false;
    int fullFrameCopiesPerFrame = 0;
    bool experimentalFastPathEnabled = false;
};

class VideoDecoderFFmpeg {
public:
    enum class RequestMode {
        Playback,
        Scrub,
        Step
    };

    VideoDecoderFFmpeg();
    ~VideoDecoderFFmpeg();

    bool open(const QString& path, QString& error);
    void close();
    bool isOpen() const;

    bool decodeFrameAt(long long frameIndex, VideoFrame& outFrame, QString& error, RequestMode mode = RequestMode::Playback);
    void setPlaybackDirection(int direction);
    void clearForwardQueue();
    void setHandoffTiming(double handoffMs);
    // Pixel size the viewer will actually draw a frame at. Only scrub previews
    // use it: converting straight to the displayed size is both cheaper and
    // sharper than converting large and letting Qt's raster bilinear take up
    // the slack. Pass an empty size to go back to the plain half-res rule.
    // Changing it clears the frame cache, whose preview entries are stored at
    // whatever size was in force when they were made.
    void setScrubPreviewSize(QSize size);

    // Hand full-resolution frames to the renderer as separate Y/U/V planes
    // instead of running swscale to BGRA, when the source is a planar YUV
    // format this can describe. Off by default: only a backend that says it can
    // sample three planes and apply the matrix itself may turn it on, and the
    // CPU backend cannot, so its path is unchanged whatever this is set to.
    //
    // SCRUB PREVIEWS ARE DELIBERATELY EXCLUDED and stay on swscale. A preview is
    // converted straight to the size it will be drawn at (`b5a56af`), which is a
    // fiftieth of the pixels of a full-resolution frame; uploading full-res
    // planes for one would move that cost onto the bus rather than remove it,
    // and playback throughput would never show it. See plan section 20.7.
    void setPlanarOutputEnabled(bool enabled);
    bool planarOutputEnabled() const { return planarOutput_; }

    long long currentFrame() const { return currentFrame_; }
    const VideoMetadata& metadata() const { return metadata_; }
    const VideoPerfStats& perfStats() const { return perfStats_; }

    // Per-phase file I/O counters. Forward playback and random access are
    // reported separately on purpose: averaged together they cannot answer
    // whether ordinary playback is read-starved.
    IoPhaseStats ioStats(IoPhase phase) const;
    StorageInfo storageInfo() const;

    // Asked, at a decoder-safe boundary inside the GOP walk, whether the frame
    // being decoded is still wanted. Returning true abandons the walk; the
    // request then fails with `wasAbandoned` set rather than with an error, and
    // no recovery seek is attempted.
    //
    // It is consulted once per packet, at the top of the walk loop, which is
    // the only point where no AVPacket is owned, no AVFrame is being written
    // and the codec is between send/receive. Do not move it: the value of the
    // whole mechanism is that the state it interrupts is quiescent.
    //
    // Called on whichever thread is driving the decoder, so it must be cheap
    // and must not touch the decoder.
    using CancelPredicate = std::function<bool()>;
    void setCancelPredicate(CancelPredicate predicate);

    // Set when the last decodeFrameAt returned false because the walk was
    // abandoned rather than because the frame could not be produced. The
    // caller uses it to tell "you no longer want this" from "this is not
    // there", which are different things and only one of them is a problem.
    bool lastRequestWasAbandoned() const { return lastRequestAbandoned_; }

    // Installed by the owner so a slow remote read keeps the event loop
    // running instead of freezing it. See MediaIoSource::StallPump.
    void setStallPump(MediaIoSource::StallPump pump);
    // Supersedes any outstanding remote read so its result is not presented.
    void cancelOutstandingIo();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    VideoMetadata metadata_;
    VideoPerfStats perfStats_;
    long long currentFrame_ = -1;
    bool lastRequestAbandoned_ = false;
    bool planarOutput_ = false;
};

} // namespace trace::core
