#pragma once

#include <QString>
#include <functional>

namespace trace::render {

// What the overlay is allowed to ask the application to do.
//
// The split is deliberate and is the rule the spike is built to respect: the
// renderer owns VISUAL GEOMETRY AND HIT REGIONS, the application owns COMMANDS
// AND STATE. Nothing here returns a handle to playback; every entry is either a
// request to run an existing action or a read of state the application already
// holds. There is no renderer-owned playback state to get out of step.
//
// Scrubbing is expressed as press / move / release rather than as a single
// "seek", so it can be routed through the real timeline slider -- which means
// the overlay inherits the entire existing scrub path, including the drag
// shuttle and the step 5.6 play-state restore, instead of reimplementing any of
// it.
struct OverlayHooks {
    // Run the centralized Play/Pause action. Not "start playback".
    std::function<void()> playPause;
    // The two side controls, each named for what it does. Spec phase 4 made the
    // forward one the shuttle and phase 5 the backward one, so the asymmetry
    // that stood between those two commits -- `stepBack` beside `fastForward` --
    // is gone and both carry the continuous-scan artwork. Frame stepping is the
    // arrow keys and has no surface here at all.
    //
    // `isVideoScrubActive()` is the standing reminder of what a name that
    // describes an intention rather than a behaviour costs.
    std::function<void()> rewind;
    std::function<void()> fastForward;

    // Timeline drag, mapped onto the slider's own press/move/release.
    std::function<void(bool)> setScrubbing;
    std::function<void(double)> seekToFraction;

    // Reads. The overlay draws from these and never caches them.
    std::function<bool()> isPlaying;
    std::function<double()> positionFraction;
    std::function<QString()> rateText;

    // Ask the host to repaint. Needed because hover, fade and drag change the
    // picture without a new video frame arriving.
    std::function<void()> requestRepaint;
};

} // namespace trace::render
