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

    // goToStart/goToEnd left this struct with their strip buttons (owner item
    // 15, 2026-08-18). Home and End reach goToFrame() through their QActions
    // directly and never needed a hook.

    // MUTE, as a command and a read. Two entries rather than one toggling
    // function returning the new state, because the glyph is drawn from
    // `isMuted` on every frame the strip is visible and the command runs only
    // on a click: a toggle that also reported would tempt the model into
    // caching what it was last told, and a cached state is one that can
    // disagree with `M` pressed from the keyboard.
    std::function<void()> toggleMute;
    std::function<bool()> isMuted;

    // VOLUME (owner decision 2026-08-20, on tester feedback, reversing step 5's
    // "mute button, no volume slider"): a read and a command, the mute shape.
    // The fraction is the UI's 0..1 -- the HOST owns the mapping to a sink gain
    // (perceptual, inside AudioOutput) and owns the muted-at-zero semantics, so
    // the model never caches a level and never talks to the audio clock. The
    // slider is NOT a picture of timelineSlider_: a volume drag must never
    // issue a seek, so it has its own press routing through these two entries
    // and touches nothing the scrub path owns.
    std::function<double()> volumeFraction;
    std::function<void(double)> setVolumeFraction;
    // The drag edges, setScrubbing's shape. The host needs them for one thing:
    // a drag that ends at zero must restore the PRE-DRAG level on the next
    // unmute click, and without the edges the host would remember the drag's
    // own ramp values (a drag to zero passes through 60, 40, 20, 5 -- and
    // "restore" would mean 5%).
    std::function<void(bool)> setVolumeDragging;

    // LOOP, the same shape as mute: a command and a read, never a cached
    // state. `loopAction_` has existed as a shared checkable QAction since spec
    // phase 14 and is already connected on `toggled` rather than `triggered`,
    // so toggle() from the strip reaches it -- which is exactly the thing that
    // was NOT true of Mute when its button was added.
    std::function<void()> toggleLoop;
    std::function<bool()> isLooping;

    // Whether the window is fullscreen, so the strip can draw the enter or the
    // exit glyph. `toggleFullscreen` below is the command and already exists --
    // the button and the double-click run the one action.
    std::function<bool()> isFullscreen;

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

    // Open the Share menu (spec phase 8), at a point in SURFACE DEVICE PIXELS.
    // The overlay knows where its button is and nothing about menus; the host
    // owns the QMenu and knows nothing about the overlay's layout, so the
    // position crosses the boundary and neither side learns the other's job.
    //
    // A menu is also the first thing the overlay opens that holds itself open:
    // `holdVisible` already answers yes for an active popup, so the panel cannot
    // fade out from under a menu it opened.
    std::function<void(int, int)> shareMenu;

    // PANNING THE ZOOMED PICTURE (spec phase 15), as a read and a command.
    //
    // It is in this struct for exactly the reason toggleFullscreen is, and the
    // reason is worth restating because it is the thing that makes the overlay
    // model the right home for a gesture that has nothing to do with the
    // transport: under the D3D11 backend -- the default -- the video is a child
    // HWND that takes every mouse message, and Qt's widget never sees one. A
    // pan implemented in ViewerWidget's mousePressEvent would work on
    // TRACE_RENDERER=cpu and do nothing at all on the shipping build.
    //
    // `canPan` is asked on the press, not cached: whether a drag can move
    // anything depends on the zoom and the window size, and both change without
    // the overlay being told. `panBy` takes a DELTA in surface device pixels,
    // so the host stores no drag anchor -- the model already owns one for the
    // timeline and a second copy of "where was the pointer last" is a second
    // thing to keep in step.
    std::function<bool()> canPan;
    std::function<void(int, int)> panBy;

    // Reads. The overlay draws from these and never caches them.
    std::function<bool()> isPlaying;
    std::function<double()> positionFraction;
    std::function<QString()> rateText;
    // THE TWO READOUTS EITHER SIDE OF THE TRACK (UI redesign roadmap step 5),
    // as finished strings.
    //
    // They are strings rather than a frame index and an fps because the FORMAT
    // is spec phase 7's and must stay there: four modes -- Frame Count,
    // Seconds, Elapsed and *source* SMPTE -- with `hasSourceTimecode_` as the
    // single gate on the fourth. An overlay that formatted its own would be a
    // fifth format, and the whole point of that phase was that elapsed time is
    // not timecode. The host builds both from the one expression the HUD uses.
    //
    // `durationText` is also what sizes the readout cells, and that is why it
    // is asked for even when it is only drawn once: the position can never be
    // wider than the duration in the same mode, so reserving the duration's
    // width keeps the track from resizing as the digits change.
    std::function<QString()> positionText;
    std::function<QString()> durationText;
    // The transient user-facing message -- what the status bar's showMessage
    // used to carry: "File path copied.", an open failure, a Go to Timecode
    // refusal. One string owned by the host (the same shape as rateText), shown
    // while non-empty and cleared by the host's own timer. It is a separate
    // hook rather than a second reader of rateText because the two can be live
    // at once -- a shuttle press while an error is still up -- and because the
    // message must outlive the panel's fade where the rate chip must not.
    std::function<QString()> messageText;

    // A veto on the auto-hide, asked each time the idle timer fires. The spec's
    // list -- a popup menu open, a tooltip open, a child control holding
    // keyboard focus -- is entirely about application state the overlay cannot
    // see, so it is asked rather than inferred. The two conditions the overlay
    // CAN see (the pointer is over a control, a timeline drag is in progress) it
    // still checks itself.
    std::function<bool()> holdVisible;

    // Show or hide the TRANSIENT TOP CHROME (UI redesign roadmap step 7), on
    // the same reveal state the floating transport uses.
    //
    // TOP AND BOTTOM ARE ONE SYSTEM. There is one idle timer, one hold list and
    // one notion of "the interface is out of the way"; this hook is how the
    // second panel learns the answer rather than computing its own. It is
    // shaped exactly like setCursorHidden, and for the same reason: the overlay
    // decides WHEN, and the host decides WHETHER -- with no media open the host
    // holds the chrome up, because there is nothing for it to get out of the way
    // of and the design's empty-state mockup shows it present.
    //
    // Called only on the transitions. "Revealed" means revealed OR still fading
    // out, so the strip appears the instant a reveal starts and leaves when the
    // panel has finished fading rather than halfway through it.
    std::function<void(bool)> setChromeRevealed;

    // Owner item 11 (2026-08-18): the model's own fade opacity, reported on
    // every animation tick, so the native top strip can fade in LOCKSTEP with
    // the composited transport -- same kFadeMs, same clock, no second timer.
    // setChromeRevealed above stays the map/unmap edge; this is the ramp
    // between its two calls. A host that does not fade simply leaves it unset.
    std::function<void(double)> setChromeOpacity;

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
