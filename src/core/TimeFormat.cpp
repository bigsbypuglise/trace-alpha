#include "core/TimeFormat.h"

#include <QRegularExpression>

#include <algorithm>
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

QString frameToElapsed(long long frame, double fps) {
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

int nominalRate(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0) return 24;
    const double r = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
    // ceil, not round. 23.976 must be 24 and 29.97 must be 30 -- rounding gets
    // both of those right and gets nothing else wrong, but ceil is the rule
    // SMPTE actually states and it also handles a container reporting 23.98.
    const int n = static_cast<int>(std::ceil(r - 0.001));
    return std::max(1, n);
}

bool dropFrameApplies(int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0) return false;
    // The 1001 families only. Compared as a ratio rather than by matching
    // literal numerators, because 30000/1001 and 60000/1001 are also written as
    // 2997/100-ish approximations by some tools, and because a file at exactly
    // 30/1 must NOT get drop-frame arithmetic however it labels itself.
    const double r = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
    return (std::abs(r - 30000.0 / 1001.0) < 0.01) || (std::abs(r - 60000.0 / 1001.0) < 0.01);
}

bool parseTimecode(const QString& text, Timecode& out) {
    // The separator before the frames field is what carries drop-frame: ':' is
    // non-drop, ';' is drop, and '.' is drop as written by some older tools.
    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d{1,2}):(\\d{1,2}):(\\d{1,2})([:;.])(\\d{1,3})\\s*$"));
    const auto m = re.match(text);
    if (!m.hasMatch()) return false;

    Timecode tc;
    tc.hours = m.captured(1).toInt();
    tc.minutes = m.captured(2).toInt();
    tc.seconds = m.captured(3).toInt();
    tc.dropFrame = (m.captured(4) != QLatin1String(":"));
    tc.frames = m.captured(5).toInt();

    // Fields are validated here and nowhere else, so an out-of-range timecode
    // cannot reach the arithmetic. Frames are NOT checked against the rate --
    // this function does not know it, and the caller that does will reject a
    // frame number the rate cannot produce.
    if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59) return false;

    out = tc;
    return true;
}

QString formatTimecode(const Timecode& tc) {
    return QString("%1:%2:%3%4%5")
        .arg(tc.hours, 2, 10, QChar('0'))
        .arg(tc.minutes, 2, 10, QChar('0'))
        .arg(tc.seconds, 2, 10, QChar('0'))
        .arg(tc.dropFrame ? QLatin1Char(';') : QLatin1Char(':'))
        .arg(tc.frames, 2, 10, QChar('0'));
}

// How many frame NUMBERS are skipped at each dropping minute: 2 at 29.97, 4 at
// 59.94. Derived from the nominal rate rather than tabled, which is the same
// relationship SMPTE states (nominal/15).
static int dropCountFor(int nominal) {
    return std::max(1, static_cast<int>(std::lround(nominal / 15.0)));
}

long long timecodeToFrames(const Timecode& tc, int fpsNum, int fpsDen) {
    const int nominal = nominalRate(fpsNum, fpsDen);
    const long long plain = static_cast<long long>(tc.hours) * 3600 * nominal
                          + static_cast<long long>(tc.minutes) * 60 * nominal
                          + static_cast<long long>(tc.seconds) * nominal
                          + tc.frames;

    if (!tc.dropFrame || !dropFrameApplies(fpsNum, fpsDen)) return std::max(0LL, plain);

    // Every minute drops `d` frame numbers except every tenth minute, so a
    // timecode's frame count is short by d x (elapsed minutes - tenth minutes).
    const int d = dropCountFor(nominal);
    const long long totalMinutes = static_cast<long long>(tc.hours) * 60 + tc.minutes;
    return std::max(0LL, plain - d * (totalMinutes - totalMinutes / 10));
}

Timecode framesToTimecode(long long frames, int fpsNum, int fpsDen, bool dropFrame) {
    const int nominal = nominalRate(fpsNum, fpsDen);
    long long f = std::max(0LL, frames);

    Timecode tc;
    tc.dropFrame = dropFrame && dropFrameApplies(fpsNum, fpsDen);

    if (tc.dropFrame) {
        // Add back the numbers the clock skips, then read the result as if it
        // were non-drop. This is the standard formulation and it is written the
        // standard way on purpose: the arithmetic is easy to get subtly wrong
        // and hard to notice, because it is correct at 00:00 and drifts by two
        // frames a minute.
        const int d = dropCountFor(nominal);
        const long long framesPerMinute = static_cast<long long>(nominal) * 60 - d;
        const long long framesPer10Minutes = static_cast<long long>(nominal) * 600 - 9 * d;

        const long long tens = f / framesPer10Minutes;
        const long long rem = f % framesPer10Minutes;
        f += 9LL * d * tens;
        // The first minute of each ten-minute block drops nothing, so a
        // remainder inside it adds nothing.
        if (rem >= d) f += d * ((rem - d) / framesPerMinute);
    }

    const long long perHour = static_cast<long long>(nominal) * 3600;
    const long long perMinute = static_cast<long long>(nominal) * 60;
    tc.hours = static_cast<int>((f / perHour) % 24);
    tc.minutes = static_cast<int>((f % perHour) / perMinute);
    tc.seconds = static_cast<int>((f % perMinute) / nominal);
    tc.frames = static_cast<int>(f % nominal);
    return tc;
}

} // namespace trace::core::TimeFormat
