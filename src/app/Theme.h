#pragma once

#include <QString>

class QApplication;

namespace trace::app {

// UI redesign roadmap step 10, the half that is not the backdrop: typography,
// spacing, restrained rounded surfaces and the accent.
//
// ONE HOME, for the same reason trace::app::settings() is one home. The
// application font, the palette and the popup-menu surface are three statements
// of the same visual language, and the way they drift is by being set in three
// places -- main.cpp had the palette, TopChrome has the strip's colours, and a
// third site for the font would have made the language something you have to
// reconstruct by grepping. TopChrome keeps its own STRIP colours because those
// are the design's screen-1 values for one surface; what lives here is what the
// rest of the application inherits.
struct Theme {
    // Font family, palette and the QMenu surface, applied to the whole
    // application. Call once, before any window is constructed.
    static void apply(QApplication& app);

    // The font family that actually resolved, for the dev HUD.
    //
    // IT IS REPORTED BECAUSE IT CAN SILENTLY BE SOMETHING ELSE, and it already
    // was once: the first build of this asked for a family Qt does not
    // enumerate, `hasFamily()` declined it, and the application ran on Segoe UI
    // looking very nearly right. This field is what said so. It is the same
    // silent-degradation class `renderer` and `planar` are reported for, and a
    // screenshot cannot tell the two apart.
    static QString resolvedFontFamily();
};

} // namespace trace::app
