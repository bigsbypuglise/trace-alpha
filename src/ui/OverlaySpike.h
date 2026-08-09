#pragma once

#include <QString>

class QWidget;

namespace trace::ui {

// TEMPORARY DIAGNOSTIC -- not interface work, and none of this artwork ships.
//
// Answers one question before the GPU initiative commits to the child-HWND
// viewer surface (plan section 17.2): can ordinary Qt widgets appear ON TOP of
// the native video rectangle and receive input there?
//
// It matters now rather than at interface time because the answer decides the
// surface design, and reversing that decision later is expensive. Trace's HUD
// and transport sit BELOW the video today (plan section 3), so nothing in the
// shipped app currently exercises this at all.
//
// Enabled with TRACE_OVERLAY_SPIKE=1. Two variants, because the interesting
// result is the difference between them:
//   1 -- plain Qt child widgets of the viewer (composited into the top-level
//        backing store)
//   2 -- the same widgets with WA_NativeWindow, so each becomes a sibling HWND
//        of the video surface and z-order is decided by the window manager
//
// Every interaction is written to stderr with an "OVERLAY-SPIKE" prefix so a
// scripted run can assert on it rather than relying on someone watching.
void installOverlaySpike(QWidget* viewer, int variant);

// 0 when the spike is off.
int overlaySpikeVariant();

} // namespace trace::ui
