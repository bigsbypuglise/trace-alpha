#include "core/ImageSequenceFrameSource.h"

namespace trace::core {

bool ImageSequenceFrameSource::frameAt(long long frameIndex, VideoFrame& outFrame, QString& error) {
    if (!loader_) {
        error = "Image loader unavailable";
        return false;
    }

    const QString path = sourcePathForFrame(frameIndex);
    if (path.isEmpty()) {
        error = "Frame index out of range";
        return false;
    }

    LoadedImageInfo info;
    if (!loader_->load(path, info, error)) return false;
    if (!info.buffer) {
        error = "Unsupported image format";
        return false;
    }

    currentFrame_ = frameIndex;
    outFrame = VideoFrame{};
    outFrame.buffer = info.buffer;
    outFrame.frameIndex = frameIndex;
    // Stills arrive already in RGB, or -- for EXR -- in scene-referred float,
    // which the display stage maps. Neither is YUV, so there is no matrix to
    // record.
    //
    // The whole LoadedImageInfo is kept so the caller can read what the file
    // actually was: its real channel count, its pass list and its compression.
    // Before this the frame-handoff path invented `channels = 4` because the
    // decoded frame was all it had to look at.
    lastInfo_ = std::move(info);
    return true;
}

long long ImageSequenceFrameSource::maxFrame() const {
    return framePaths_.isEmpty() ? -1 : static_cast<long long>(framePaths_.size()) - 1;
}

bool ImageSequenceFrameSource::canPlay() const {
    return maxFrame() > 0;
}

QString ImageSequenceFrameSource::sourcePathForFrame(long long frameIndex) const {
    if (frameIndex < 0 || frameIndex >= static_cast<long long>(framePaths_.size())) return {};
    return framePaths_[static_cast<int>(frameIndex)];
}

} // namespace trace::core
