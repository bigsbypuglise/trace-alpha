#pragma once

#include "core/PlaybackState.h"

namespace trace::core {

// Where a shuttle press enters the 1x/2x/5x/10x/30x ladder from a stop, or from
// the opposite direction. The ladder above the first rung is identical either
// way; only the way IN differs.
//
// TWO CONVENTIONS, BOTH OWNER-CONFIRMED (2026-08-10), and they do not agree:
//
//   AtOneX  -- the J/K/L keyboard convention. The first press is 1x, which is
//              ordinary playback on the validated audio-mastered path; only the
//              rungs above it shuttle at all.
//   AtTwoX  -- the Rewind / Fast-forward BUTTON convention, which the interface
//              spec states directly: "If paused, the first press begins forward
//              playback at +2x." A scan button whose first click produced
//              ordinary 1x playback would read as not having worked.
//
// This exists so the difference is a documented argument rather than a call site
// reaching past the controller to write `speed`. The ladder has one
// implementation and the state machine keeps one owner.
enum class ShuttleEntry { AtOneX, AtTwoX };

class PlaybackController {
public:
    void resetForNewMedia(long long maxFrame);
    void setCurrentFrame(long long frame);
    void togglePlayPause();
    void pause();
    void jogForward(ShuttleEntry entry = ShuttleEntry::AtOneX);
    void jogReverse(ShuttleEntry entry = ShuttleEntry::AtOneX);
    // Forward at a rate the user named outright, rather than a rung reached by
    // pressing. See the definition for why this is not a hole in "the
    // controller owns the ladder".
    void playForwardAt(double speed);
    void stepForward();
    void stepBackward();

    const PlaybackState& state() const noexcept { return state_; }

private:
    void clamp();
    PlaybackState state_{};
};

} // namespace trace::core
