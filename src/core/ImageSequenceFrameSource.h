#pragma once

#include <QString>
#include <QStringList>
#include <utility>

#include "core/FrameSource.h"
#include "core/StillImageLoader.h"

namespace trace::core {

class ImageSequenceFrameSource final : public FrameSource {
public:
    ImageSequenceFrameSource(StillImageLoader* loader, QStringList framePaths, double fps = 24.0)
        : loader_(loader), framePaths_(std::move(framePaths)), fps_(fps) {}

    bool frameAt(long long frameIndex, VideoFrame& outFrame, QString& error) override;
    double fps() const override { return fps_; }
    long long currentFrame() const override { return currentFrame_; }
    void setCurrentFrame(long long frame) override { currentFrame_ = frame; }
    long long maxFrame() const override;
    bool canPlay() const override;
    QString sourcePathForFrame(long long frameIndex) const override;

    // What the last successfully loaded frame's file actually was. Empty until
    // the first load succeeds.
    const LoadedImageInfo& lastInfo() const { return lastInfo_; }

    // Pass selection belongs to the loader (it must be identical for every frame
    // of a sequence); this is the route to it from a caller that only holds the
    // source.
    StillImageLoader* loader() const { return loader_; }

private:
    StillImageLoader* loader_ = nullptr;
    QStringList framePaths_;
    double fps_ = 24.0;
    long long currentFrame_ = 0;
    LoadedImageInfo lastInfo_;
};

} // namespace trace::core
