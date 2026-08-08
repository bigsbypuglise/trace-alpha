#pragma once

#include <QString>

#include "core/VideoFrame.h"

namespace trace::core {

class FrameSource {
public:
    virtual ~FrameSource() = default;

    // On success `outFrame` holds pixels the caller may keep for as long as it
    // likes -- ownership is a refcount, not a borrow of decoder state.
    virtual bool frameAt(long long frameIndex, VideoFrame& outFrame, QString& error) = 0;
    virtual double fps() const = 0;
    virtual long long currentFrame() const = 0;
    virtual void setCurrentFrame(long long frame) = 0;
    virtual long long maxFrame() const = 0;
    virtual bool canPlay() const = 0;
    virtual QString sourcePathForFrame(long long frameIndex) const = 0;
};

} // namespace trace::core
