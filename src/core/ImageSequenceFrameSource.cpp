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

    // `info` is local and its image is freshly loaded, so this moves rather than
    // copies: adopt() only has to detach when the caller kept a reference.
    auto buffer = FrameBuffer::adopt(std::move(info.image));
    if (!buffer) {
        error = "Unsupported image format";
        return false;
    }

    currentFrame_ = frameIndex;
    outFrame = VideoFrame{};
    outFrame.buffer = std::move(buffer);
    outFrame.frameIndex = frameIndex;
    // Stills arrive already in RGB; there is no YUV matrix to record.
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
