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

// The rewind ladder is 1x, 2x, 5x, 10x, 30x -- the speeds the planned interface
// asks for, rather than the doubling the forward jog uses.
//
// It is a ladder of SAMPLING STRIDES, not of tick rates. The reverse shuttle
// presents one frame per source-frame period at every speed and changes which
// frames those are, so the speed is the distance between presented frames. That
// is what keeps the cadence identical at 1x and at 30x, and it is why the stride
// can be the commanded speed rather than something inferred from how the decoder
// is coping: nothing measured feeds back into it, so it cannot run away the way
// three of the four scrub-gate inferences did.
void PlaybackController::jogReverse() {
    state_.mode = PlaybackMode::PlayingReverse;
    static constexpr double kRewindLadder[] = {-1.0, -2.0, -5.0, -10.0, -30.0};
    if (state_.speed >= 0.0) {
        state_.speed = kRewindLadder[0];
        return;
    }
    for (double step : kRewindLadder) {
        if (step < state_.speed) {   // more negative == faster
            state_.speed = step;
            return;
        }
    }
    // Already at the top of the ladder: stay there rather than wrapping.
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
