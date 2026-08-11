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

    // The source rate as an exact rational, when the container carries one.
    //
    // Every other consumer takes fps() and is right to: a double is what
    // timecode, seek arithmetic and the HUD want. The presentation scheduler is
    // the one caller that cannot use it, because 24000/1001 as a double is
    // already an approximation and a cadence measured against an approximate
    // rate cannot distinguish drift from the thing being measured.
    //
    // Returns false when there is no rational to give -- image sequences have a
    // nominal rate rather than a container one -- and the caller then falls back
    // to fps() with no other change in behaviour.
    virtual bool fpsRational(int& num, int& den) const {
        num = 0;
        den = 0;
        return false;
    }

    // The source's SMPTE start timecode, when it carries one.
    //
    // Returns false when there is none, and that is a real answer rather than a
    // gap to be filled: the spec forbids generating SMPTE from zero when the
    // source states none, and forbids labelling an elapsed-time conversion as
    // source timecode. An image sequence has no container timecode at all, so
    // the default here is the honest answer for it and it takes no override --
    // the same shape as fpsRational, and for the same reason.
    virtual bool sourceTimecode(QString& start, bool& dropFrame) const {
        start.clear();
        dropFrame = false;
        return false;
    }

    virtual long long currentFrame() const = 0;
    virtual void setCurrentFrame(long long frame) = 0;
    virtual long long maxFrame() const = 0;
    virtual bool canPlay() const = 0;
    virtual QString sourcePathForFrame(long long frameIndex) const = 0;
};

} // namespace trace::core
