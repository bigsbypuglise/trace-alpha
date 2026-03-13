#pragma once

namespace trace::core {

enum class PlaybackMode { Empty, Paused, PlayingForward, PlayingReverse };

struct PlaybackState {
    PlaybackMode mode = PlaybackMode::Empty;
    long long currentFrame = 0;
    long long maxFrame = -1;
    double speed = 0.0;
};

} // namespace trace::core
