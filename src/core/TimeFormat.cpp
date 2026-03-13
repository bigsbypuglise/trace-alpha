#include "core/TimeFormat.h"

#include <cmath>

namespace trace::core::TimeFormat {

static double normalizedFps(double fps) {
    if (fps <= 0.0) return 24.0;
    return fps;
}

double frameToSeconds(long long frame, double fps) {
    const double f = normalizedFps(fps);
    return static_cast<double>(frame) / f;
}

QString formatSeconds(double seconds, int decimals) {
    return QString::number(seconds, 'f', decimals);
}

QString frameToTimecode(long long frame, double fps) {
    const double f = normalizedFps(fps);
    const long long totalFrames = std::max(0LL, frame);

    const long long fpsInt = static_cast<long long>(std::llround(f));
    const long long frames = (fpsInt > 0) ? (totalFrames % fpsInt) : 0;

    const long long totalSeconds = static_cast<long long>(std::floor(static_cast<double>(totalFrames) / f));
    const long long seconds = totalSeconds % 60;
    const long long minutes = (totalSeconds / 60) % 60;
    const long long hours = totalSeconds / 3600;

    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

} // namespace trace::core::TimeFormat
