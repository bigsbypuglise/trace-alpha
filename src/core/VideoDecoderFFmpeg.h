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

class VideoDecoderFFmpeg {
public:
    VideoDecoderFFmpeg();
    ~VideoDecoderFFmpeg();

    bool open(const QString& path, QString& error);
    void close();
    bool isOpen() const;

    bool decodeFrameAt(long long frameIndex, QImage& outImage, QString& error);

    long long currentFrame() const { return currentFrame_; }
    const VideoMetadata& metadata() const { return metadata_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    VideoMetadata metadata_;
    long long currentFrame_ = -1;
};

} // namespace trace::core
