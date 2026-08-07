#pragma once

#include <QString>
#include <QImage>

namespace trace::core {

struct VideoMetadata {
    int width = 0;
    int height = 0;
    double fps = 24.0;
    long long frameCount = -1;
    double durationSeconds = 0.0;
    QString codecName;
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

    long long seekSamples = 0;
    long long samples = 0;
    long long reverseCacheHits = 0;
    long long reverseCacheLookups = 0;
    long long forwardQueueHits = 0;
    long long forwardQueueMisses = 0;
    int forwardQueueDepth = 0;
    int forwardQueueCapacity = 0;
    long long lateFrames = 0;

    QString srcPixelFormat;
    QString dstPixelFormat;
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

    bool decodeFrameAt(long long frameIndex, QImage& outImage, QString& error, RequestMode mode = RequestMode::Playback);
    void setPlaybackDirection(int direction);
    void clearForwardQueue();
    void setHandoffTiming(double handoffMs);

    long long currentFrame() const { return currentFrame_; }
    const VideoMetadata& metadata() const { return metadata_; }
    const VideoPerfStats& perfStats() const { return perfStats_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    VideoMetadata metadata_;
    VideoPerfStats perfStats_;
    long long currentFrame_ = -1;
};

} // namespace trace::core
