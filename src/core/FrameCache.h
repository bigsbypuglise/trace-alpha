#pragma once

#include <QImage>
#include <QString>
#include <unordered_map>
#include <optional>

namespace trace::core {

struct CachedFrame {
    long long frameIndex = -1;
    QString path;
    QImage image;
    int width = 0;
    int height = 0;
    int channels = 0;
};

class FrameCache {
public:
    explicit FrameCache(int windowRadius = 1);

    void clear();
    void setWindowCenter(long long centerFrame);

    std::optional<CachedFrame> get(long long frameIndex) const;
    void put(const CachedFrame& frame);

private:
    bool inWindow(long long frameIndex) const;

    int radius_ = 1;
    long long center_ = 0;
    std::unordered_map<long long, CachedFrame> frames_;
};

} // namespace trace::core
