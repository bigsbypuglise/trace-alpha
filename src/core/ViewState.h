#pragma once

namespace trace::core {

enum class PrimaryReadoutMode {
    Frame,
    Seconds,
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
    bool showHud = true;
    bool fullscreen = false;
    PrimaryReadoutMode readoutMode = PrimaryReadoutMode::Frame;
};

} // namespace trace::core
