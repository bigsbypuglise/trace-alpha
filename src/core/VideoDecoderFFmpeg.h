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
    long long seekSamples = 0;
    long long samples = 0;
    long long reverseCacheHits = 0;
    long long reverseCacheLookups = 0;
    long long forwardQueueHits = 0;
    long long forwardQueueMisses = 0;
    int forwardQueueDepth = 0;
    int forwardQueueCapacity = 0;
    long long lateFrames = 0;
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
