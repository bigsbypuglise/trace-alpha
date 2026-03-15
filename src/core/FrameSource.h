#pragma once

#include <QImage>
#include <QString>

namespace trace::core {

class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool frameAt(long long frameIndex, QImage& outImage, QString& error) = 0;
    virtual double fps() const = 0;
    virtual long long currentFrame() const = 0;
    virtual void setCurrentFrame(long long frame) = 0;
    virtual long long maxFrame() const = 0;
    virtual bool canPlay() const = 0;
    virtual QString sourcePathForFrame(long long frameIndex) const = 0;
};

} // namespace trace::core
