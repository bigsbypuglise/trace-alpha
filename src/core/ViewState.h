#pragma once

namespace trace::core {

// The spec's Time Display list is Elapsed Time, SMPTE Timecode and Frame Count.
// `Seconds` is Trace's own and predates the spec; it is kept because `S` is an
// existing binding and the rule on conflict is to preserve one.
//
// ELAPSED AND TIMECODE ARE SEPARATE MODES AND THAT IS THE WHOLE POINT OF PHASE
// 7. Before it there was one mode called `Timecode` that printed an elapsed-time
// conversion of the frame index for every file -- including files carrying a
// real start timecode, whose value it ignored, and files carrying none, for
// which it invented one. The spec forbids both halves of that. `Elapsed` is the
// honest name for what that code computed; `Timecode` now means the source's,
// and is selectable ONLY when the source has one.
enum class PrimaryReadoutMode {
    Frame,
    Seconds,
    Elapsed,
    Timecode
};

// Only what something actually reads.
//
// `showTimecode`, `showSeconds` and `showInfo` were removed on 2026-08-10:
// nothing read any of the three, and `showInfo` had a key bound to it, so
// pressing `I` flipped a bool, repainted, and changed nothing on screen. A flag
// with a shortcut and no reader is worse than no flag -- it reads as a feature
// that is broken rather than as one that does not exist. The readout the first
// two were named for is `readoutMode`, which is real; the diagnostics HUD the
// third was named for is `showHud`, which is real and is now on `H`.
struct ViewState {
    // FALSE since the UI redesign roadmap's step 2 (2026-08-17): the dev HUD is
    // an instrument, and the shipping stage is clean. `H` (and Return/Enter)
    // toggles it, View > Show Diagnostics HUD is the discoverable surface, and
    // TRACE_HUD=1 forces it on from launch for the measurement harnesses --
    // which restart.ps1 passes by default, because every recorded figure is
    // read off the HUD. Flipping this default moves the video rect and every
    // stall figure with it (section 22.8); the re-baseline was taken with it
    // false and TRACE_HUD=1 on the harness side.
    bool showHud = false;
    bool fullscreen = false;
    PrimaryReadoutMode readoutMode = PrimaryReadoutMode::Frame;
};

} // namespace trace::core
