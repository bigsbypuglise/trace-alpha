#include "core/FrameCache.h"

namespace trace::core {

FrameCache::FrameCache(int windowRadius) : radius_(windowRadius) {}

void FrameCache::clear() {
    frames_.clear();
}

void FrameCache::setWindowCenter(long long centerFrame) {
    center_ = centerFrame;

    for (auto it = frames_.begin(); it != frames_.end();) {
        if (!inWindow(it->first)) it = frames_.erase(it);
        else ++it;
    }
}

std::optional<CachedFrame> FrameCache::get(long long frameIndex) const {
    const auto it = frames_.find(frameIndex);
    if (it == frames_.end()) return std::nullopt;
    return it->second;
}

void FrameCache::put(const CachedFrame& frame) {
    if (!inWindow(frame.frameIndex)) return;
    frames_[frame.frameIndex] = frame;
}

bool FrameCache::inWindow(long long frameIndex) const {
    return frameIndex >= (center_ - radius_) && frameIndex <= (center_ + radius_);
}

} // namespace trace::core
