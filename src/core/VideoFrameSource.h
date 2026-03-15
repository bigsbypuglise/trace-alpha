#pragma once

#include "core/FrameSource.h"
#include "core/VideoDecoderFFmpeg.h"

namespace trace::core {

class VideoFrameSource final : public FrameSource {
public:
    explicit VideoFrameSource(VideoDecoderFFmpeg* decoder) : decoder_(decoder) {}

    bool frameAt(long long frameIndex, QImage& outImage, QString& error) override;
    double fps() const override;
    long long currentFrame() const override { return currentFrame_; }
    void setCurrentFrame(long long frame) override { currentFrame_ = frame; }
    long long maxFrame() const override;
    bool canPlay() const override;
    QString sourcePathForFrame(long long frameIndex) const override;

private:
    VideoDecoderFFmpeg* decoder_ = nullptr;
    long long currentFrame_ = 0;
};

} // namespace trace::core
