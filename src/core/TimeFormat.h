#pragma once

#include <QString>

namespace trace::core::TimeFormat {

double frameToSeconds(long long frame, double fps);
QString formatSeconds(double seconds, int decimals = 3);
QString frameToTimecode(long long frame, double fps);

} // namespace trace::core::TimeFormat
