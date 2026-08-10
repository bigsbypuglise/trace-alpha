#include "core/VideoFrameSource.h"

#include <QFileInfo>

#include <algorithm>

namespace trace::core {

bool VideoFrameSource::frameAt(long long frameIndex, VideoFrame& outFrame, QString& error) {
    if (!decoder_) {
        error = "Video source unavailable";
        return false;
    }
    currentFrame_ = frameIndex;
    return decoder_->decodeFrameAt(frameIndex, outFrame, error, requestMode_);
}

double VideoFrameSource::fps() const {
    if (!decoder_) return 24.0;
    return std::max(1.0, decoder_->metadata().fps);
}

bool VideoFrameSource::fpsRational(int& num, int& den) const {
    num = 0;
    den = 0;
    if (!decoder_) return false;
    const auto& md = decoder_->metadata();
    // Both halves must be positive. av_guess_frame_rate returns 0/0 when it
    // cannot tell, and metadata_ keeps its 24/1 default in that case -- which is
    // a guess, not a container fact, but it is the same guess fps() makes and
    // the scheduler wants the two to agree.
    if (md.fpsNum <= 0 || md.fpsDen <= 0) return false;
    num = md.fpsNum;
    den = md.fpsDen;
    return true;
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
