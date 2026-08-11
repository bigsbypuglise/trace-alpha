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

    // Double-click the video toggles fullscreen (spec phase 6). A WINDOW command
    // rather than a transport one, and it is here because this struct is the
    // application's whole input contract for the video surface -- under the
    // D3D11 backend the child HWND takes every mouse message and Qt sees none of
    // them, so a second route would have to reimplement that plumbing to reach
    // the same action.
    std::function<void()> toggleFullscreen;

    // Reads. The overlay draws from these and never caches them.
    std::function<bool()> isPlaying;
    std::function<double()> positionFraction;
    std::function<QString()> rateText;

    // A veto on the auto-hide, asked each time the idle timer fires. The spec's
    // list -- a popup menu open, a tooltip open, a child control holding
    // keyboard focus -- is entirely about application state the overlay cannot
    // see, so it is asked rather than inferred. The two conditions the overlay
    // CAN see (the pointer is over a control, a timeline drag is in progress) it
    // still checks itself.
    std::function<bool()> holdVisible;

    // Hide or show the pointer. Called only on the transitions, never per move.
    // The overlay decides WHEN -- the same inactivity that fades the panel --
    // and the host decides WHETHER, because the spec hides the cursor in
    // fullscreen only.
    std::function<void(bool)> setCursorHidden;

    // Ask the host to repaint. Needed because hover, fade and drag change the
    // picture without a new video frame arriving.
    std::function<void()> requestRepaint;
};

} // namespace trace::render
