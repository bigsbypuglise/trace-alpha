#pragma once

#include <QString>
#include <QSize>
#include <cmath>
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

    // The source's SMPTE start timecode, exactly as the container states it,
    // and EMPTY when the container states none. Nothing here is ever
    // synthesised: the spec's rule is *"do not generate SMPTE from zero when
    // none exists"*, and an absent timecode has to stay absent all the way to
    // the readout rather than becoming 00:00:00:00 somewhere in the middle.
    //
    // Stored as the parsed-and-revalidated string rather than the raw
    // dictionary value, so anything unreadable has already become "no timecode"
    // by the time it leaves the decoder. A wrong timecode is worse than none in
    // a review tool.
    QString startTimecode;
    // A property of the TIMECODE rather than of the rate -- 29.97 material
    // legitimately carries either, and the container's separator says which.
    bool startTimecodeDropFrame = false;
    bool hasStartTimecode = false;

    // HOW THE SOURCE IS MEANT TO BE SHOWN, WHICH IS NOT width/height (spec
    // section 4, whose first requirement says so outright: "do not assume
    // display ratio is always encoded width divided by encoded height").
    //
    // The sample (pixel) aspect ratio actually in force, as FFmpeg composes it
    // from the codec's SAR, the container's, and any display-aspect metadata the
    // container states -- av_guess_sample_aspect_ratio is that composition, and
    // is why there is no separate "DAR metadata" field here. 1/1 when nothing
    // states otherwise, never 0/0 or 0/1: an unknown SAR means square pixels,
    // and a zero would silently turn the ratio into a divide by zero at the one
    // place it is used.
    int sarNum = 1;
    int sarDen = 1;
    // True when the container stated a SAR at all, as opposed to Trace assuming
    // square pixels. Reported rather than inferred, for the same reason
    // colorMatrixInferred is: "1:1 because the file says so" and "1:1 because
    // nobody said" are different claims and only one of them is evidence.
    bool sarStated = false;

    // Clockwise rotation the container asks for, from the display matrix,
    // normalised to 0/90/180/270. Rotation metadata is orientation, not pixels:
    // the frame is decoded exactly as encoded and this says which way up it is
    // meant to be seen.
    int rotationDegrees = 0;
    // The display matrix said something that is not a multiple of 90 (a real if
    // rare thing -- it is a general affine). It is snapped to the nearest
    // quarter turn and this records that it was, because a player that silently
    // squares off an arbitrary rotation and one that had nothing to rotate look
    // identical from the outside.
    bool rotationSnapped = false;

    // The display aspect ratio as a double: encoded size corrected by SAR, then
    // transposed if the rotation metadata turns the picture on its side.
    //
    // The VIEW transform is deliberately NOT folded in here. This is a property
    // of the media; Rotate Left/Right is a property of the session, lives on
    // ViewTransform, and is composed on top by whoever needs the on-screen
    // ratio. Mixing the two would mean a file's shape changed when a menu item
    // was ticked, and the aspect lock has to be able to tell those apart to
    // restore "the original media ratio when transforms reset".
    double displayAspect() const {
        if (width <= 0 || height <= 0) return 0.0;
        const double sar = (sarNum > 0 && sarDen > 0)
                               ? static_cast<double>(sarNum) / static_cast<double>(sarDen)
                               : 1.0;
        const double a = (static_cast<double>(width) * sar) / static_cast<double>(height);
        if (a <= 0.0) return 0.0;
        return (rotationDegrees == 90 || rotationDegrees == 270) ? 1.0 / a : a;
    }
    // WHAT THE FILE STATES, FOR THE MOVIE INSPECTOR (spec phase 13).
    //
    // Everything in this block is read from the container and stored VERBATIM,
    // including its absence. The spec's rule for the inspector is "display
    // Unknown or Untagged honestly; do not infer missing colour metadata inside
    // the inspector; distinguish encoded metadata from playback inference", and
    // that last clause is why these exist at all rather than the inspector
    // reading VideoPerfStats.
    //
    // `VideoPerfStats::colorMatrix` is the matrix PLAYBACK USED. When a file
    // states none, swsCoefficientsFor applies the standard "HD and up is 709"
    // heuristic and sets colorMatrixInferred -- which is correct for decoding
    // and is an *answer Trace invented*. An inspector that printed it would be
    // telling the user their file is tagged BT.709 when it is tagged nothing,
    // and in a review tool that is a bug report about the media rather than
    // about Trace. So the two are kept apart at the source: these say what the
    // file says, that says what was done about it, and the inspector can show
    // both.
    //
    // Empty means the container stated nothing. Never a default, never a guess.
    QString taggedColorPrimaries;
    QString taggedColorTransfer;
    QString taggedColorMatrix;
    // Tri-state deliberately, because "limited" is both a real tag and the
    // fallback assumption: false/false is untagged, and a bool could not say so.
    bool taggedRangeIsFull = false;
    bool hasTaggedRange = false;

    // Container, codec profile and track identity -- all "Unknown" as an empty
    // string rather than as an invented value.
    QString containerName;
    QString containerLongName;
    QString codecProfile;
    // The container's own track id, which is what an editorial tool means by
    // "track ID", not FFmpeg's array position. Both are kept: the index is what
    // every log line here already prints.
    int videoStreamIndex = -1;
    int videoTrackId = -1;
    // Stream bitrate, distinct from the file's overall data rate. -1 when the
    // container does not state one, which is common in MOV.
    long long videoBitrateBps = -1;

    // THE ENCODED PIXEL FORMAT AND ITS DEPTH, AND NEITHER OF THE TWO OBVIOUS
    // SOURCES IS USABLE FOR EITHER -- which is the same trap as the colour tags
    // above, in a different costume.
    //
    // `VideoPerfStats::srcPixelFormat` is rewritten by every conversion and
    // gains a " (a-skip)" suffix when the alpha plane is dropped, so it is what
    // playback last DID rather than what the file IS. And
    // `VideoPerfStats::srcBitDepth` is av_get_bits_per_pixel(), which is bits
    // per PIXEL: it reads 12 for 8-bit yuv420p, so an inspector row labelled
    // "Bit depth" built from it would tell the user an 8-bit H.264 file is
    // 12-bit. In a review tool that is a bug report about the media.
    //
    // So the format is read once at open, from the descriptor, and the depth is
    // taken from the component rather than from the pixel. Both are kept,
    // because both are real and they are different numbers.
    QString pixelFormatName;
    int bitsPerComponent = 0;
    int bitsPerPixel = 0;

    bool hasAudio = false;
    QString audioCodecName;
    QString audioProfile;
    QString audioChannelLayout;
    int audioSampleRate = 0;
    int audioChannels = 0;
    long long audioBitrateBps = -1;
    int audioStreamIndex = -1;
    int audioTrackId = -1;

    // The size the source is meant to be shown at with square pixels and no
    // scaling -- section 4's "natural displayed size", which is what the window
    // is sized from on open. Rotation is applied; the view transform is not,
    // for the reason above.
    QSize naturalDisplaySize() const {
        if (width <= 0 || height <= 0) return QSize();
        const double sar = (sarNum > 0 && sarDen > 0)
                               ? static_cast<double>(sarNum) / static_cast<double>(sarDen)
                               : 1.0;
        // Widen rather than narrow when SAR > 1, and heighten when SAR < 1: a
        // natural size that discards resolution would make "do not upscale small
        // media" throw away detail the file has.
        int w = width;
        int h = height;
        if (sar >= 1.0) w = static_cast<int>(std::lround(width * sar));
        else h = static_cast<int>(std::lround(height / sar));
        if (rotationDegrees == 90 || rotationDegrees == 270) return QSize(h, w);
        return QSize(w, h);
    }
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
    //
    // Returns HOW MANY entries that discarded, which is the only honest measure
    // of what a resize costs the cache: a clear of an already-empty cache is
    // free, so a count of clears says nothing and a count of discarded entries
    // says everything. An unchanged size returns 0 without touching anything.
    int setScrubPreviewSize(QSize size);

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

    // An RGB copy of an already-decoded frame, for Copy Current Frame (spec
    // phase 14). Returns false and leaves `out` null on any failure.
    //
    // IT EXISTS BECAUSE SINCE GATE C THERE IS NO RGB ANYWHERE ON THE SHIPPING
    // PATH. A full-resolution frame on d3d11 is three YUV planes and the matrix
    // is applied in the pixel shader, so VideoFrame::toQImage() returns a null
    // image for one by construction -- qtFormatFor() refuses planar layouts on
    // purpose, so that a luma plane can never be shown as garbage. Copying the
    // picture therefore needs a conversion that the presentation path no longer
    // performs.
    //
    // It takes the FRAME, not an index, so what is copied is exactly what is on
    // screen -- asking the decoder to re-decode the current index would pay a
    // seek, could land differently after a cache hit, and would be a second
    // answer to "which frame is displayed".
    //
    // Its swscale context is created and destroyed per call and never enters
    // the four-slot LRU. This runs once per keypress and costs a few
    // milliseconds; letting it evict a slot would make the next playback frame
    // pay a context rebuild, which is the cost `5e57d86` exists to avoid.
    bool frameToRgbImage(const VideoFrame& frame, QImage& out, QString& error) const;

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
