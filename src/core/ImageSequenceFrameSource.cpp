#include "core/ImageSequenceFrameSource.h"

namespace trace::core {

bool ImageSequenceFrameSource::frameAt(long long frameIndex, QImage& outImage, QString& error) {
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
    currentFrame_ = frameIndex;
    outImage = info.image;
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
