#include "core/PlaybackController.h"

namespace trace::core {

void PlaybackController::resetForNewMedia(long long maxFrame) {
    state_ = {};
    state_.mode = PlaybackMode::Paused;
    state_.maxFrame = maxFrame;
}

void PlaybackController::setCurrentFrame(long long frame) {
    state_.currentFrame = frame;
    clamp();
}

void PlaybackController::togglePlayPause() {
    if (state_.mode == PlaybackMode::PlayingForward || state_.mode == PlaybackMode::PlayingReverse) {
        pause();
    } else {
        state_.mode = PlaybackMode::PlayingForward;
        state_.speed = 1.0;
    }
}

void PlaybackController::pause() {
    state_.mode = PlaybackMode::Paused;
    state_.speed = 0.0;
}

void PlaybackController::jogForward() {
    state_.mode = PlaybackMode::PlayingForward;
    if (state_.speed <= 0.0) state_.speed = 1.0;
    else if (state_.speed < 4.0) state_.speed *= 2.0;
}

void PlaybackController::jogReverse() {
    state_.mode = PlaybackMode::PlayingReverse;
    if (state_.speed >= 0.0) state_.speed = -1.0;
    else if (state_.speed > -4.0) state_.speed *= 2.0;
}

void PlaybackController::stepForward() {
    state_.mode = PlaybackMode::Paused;
    state_.speed = 0.0;
    ++state_.currentFrame;
    clamp();
}

void PlaybackController::stepBackward() {
    state_.mode = PlaybackMode::Paused;
    state_.speed = 0.0;
    --state_.currentFrame;
    clamp();
}

void PlaybackController::clamp() {
    if (state_.currentFrame < 0) state_.currentFrame = 0;
    if (state_.maxFrame >= 0 && state_.currentFrame > state_.maxFrame) state_.currentFrame = state_.maxFrame;
}

} // namespace trace::core
