#include "core/VideoFrameSource.h"

#include <QFileInfo>

#include <algorithm>

namespace trace::core {

bool VideoFrameSource::frameAt(long long frameIndex, QImage& outImage, QString& error) {
    if (!decoder_) {
        error = "Video source unavailable";
        return false;
    }
    currentFrame_ = frameIndex;
    return decoder_->decodeFrameAt(frameIndex, outImage, error);
}

double VideoFrameSource::fps() const {
    if (!decoder_) return 24.0;
    return std::max(1.0, decoder_->metadata().fps);
}

long long VideoFrameSource::maxFrame() const {
    if (!decoder_) return -1;
    return decoder_->metadata().frameCount > 0 ? decoder_->metadata().frameCount - 1 : -1;
}

bool VideoFrameSource::canPlay() const {
    return maxFrame() > 0;
}

QString VideoFrameSource::sourcePathForFrame(long long) const {
    return {};
}

} // namespace trace::core
