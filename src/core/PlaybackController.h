#pragma once

#include "core/PlaybackState.h"

namespace trace::core {

class PlaybackController {
public:
    void resetForNewMedia(long long maxFrame);
    void setCurrentFrame(long long frame);
    void togglePlayPause();
    void pause();
    void jogForward();
    void jogReverse();
    void stepForward();
    void stepBackward();

    const PlaybackState& state() const noexcept { return state_; }

private:
    void clamp();
    PlaybackState state_{};
};

} // namespace trace::core
