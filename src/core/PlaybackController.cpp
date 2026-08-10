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

// The forward ladder is 1x, 2x, 5x, 10x, 30x -- the same rungs as rewind, and
// for the same reason: above 1x the speed is a SAMPLING STRIDE, not a tick rate.
//
// It used to double and cap at 4x, which failed in two separate ways at once.
// The ceiling made 5x, 10x and 30x unreachable on every format; and because the
// speed was carried in the tick rate with one frame presented per tick, the
// achieved speed was capped by per-frame decode cost rather than by the request.
// Measured before the change: ProRes 4444 asked for 2x and delivered 1.00x, then
// asked for 4x and delivered 1.33x -- so two rungs of the ladder were visually
// identical and neither was the speed on the label.
//
// The first press from a stop stays exactly 1x on the KEYBOARD convention,
// which is ordinary playback on the validated audio-mastered path; only the
// rungs above it shuttle. The BUTTON convention enters at 2x instead -- see
// ShuttleEntry, and note the two are deliberately different rather than one of
// them being a bug.
void PlaybackController::jogForward(ShuttleEntry entry) {
    state_.mode = PlaybackMode::PlayingForward;
    static constexpr double kShuttleLadder[] = {1.0, 2.0, 5.0, 10.0, 30.0};
    if (state_.speed <= 0.0) {
        // Coming from a stop or from reverse: reset to the bottom of the ladder
        // rather than mirroring whatever the other direction had reached.
        //
        // The entry convention is applied HERE and only here -- it decides the
        // first rung, never any later one, so a button run and a keyboard run
        // that have both reached 5x behave identically from then on.
        state_.speed = entry == ShuttleEntry::AtTwoX ? kShuttleLadder[1] : kShuttleLadder[0];
        return;
    }
    for (double step : kShuttleLadder) {
        if (step > state_.speed) {
            state_.speed = step;
            return;
        }
    }
    // Already at the top: stay there rather than wrapping.
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
void PlaybackController::jogReverse(ShuttleEntry entry) {
    state_.mode = PlaybackMode::PlayingReverse;
    static constexpr double kRewindLadder[] = {-1.0, -2.0, -5.0, -10.0, -30.0};
    if (state_.speed >= 0.0) {
        state_.speed = entry == ShuttleEntry::AtTwoX ? kRewindLadder[1] : kRewindLadder[0];
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
