#pragma once

namespace trace::core {

enum class PrimaryReadoutMode {
    Frame,
    Seconds,
    Timecode
};

struct ViewState {
    bool showHud = true;
    bool fullscreen = false;
    bool showTimecode = true;
    bool showSeconds = true;
    bool showInfo = true;
    PrimaryReadoutMode readoutMode = PrimaryReadoutMode::Frame;
};

} // namespace trace::core
