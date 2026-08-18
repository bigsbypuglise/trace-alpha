#pragma once

#include <QObject>
#include <QString>
#include <vector>

QT_BEGIN_NAMESPACE
class QAction;
class QWidget;
QT_END_NAMESPACE

namespace trace::render {
class OverlayModel;
}

namespace trace::app {

// THE ACCESSIBILITY PROXY TREE OVER THE COMPOSITED TRANSPORT (spec phase 14,
// plan section 19.7).
//
// The problem it solves, stated plainly: Qt exposes standard widgets to UI
// Automation automatically, so Trace needed no accessibility code at all while
// the transport was TransportBar's QPushButtons and QSlider. Spec phase 6 made
// the composited overlay the default and took the bar out of the layout -- and
// the composited overlay has NO WIDGET TREE, so it exposes nothing. The
// shipping build's transport went from automatically accessible to invisible to
// a screen reader, in one commit, with nothing on screen to say so.
//
// The design is plan section 19.7's, which was written as a plan and never
// prototyped: one zero-painting, input-transparent QWidget per control,
// positioned on the same rects the compositor lays out, with names and
// shortcuts read from the QActions the overlay's hooks already call.
//
// FOUR PROPERTIES ARE LOAD-BEARING.
//
//   1. The geometry comes from OverlayModel::controlRects(), which returns the
//      rects layout() computed and buildFrame() drew from. A proxy tree built
//      from its own layout would agree on the day it was written and drift
//      silently -- and a screen reader announcing a button that moved two
//      phases ago is not something anyone would notice from the picture.
//
//   2. Qt::WA_TransparentForMouseEvents on every proxy. Under the D3D11 backend
//      the surface is a child HWND that takes every mouse message and Qt sees
//      none of them; under the CPU backend the viewer handles them itself. A
//      proxy that accepted a click would break the hit test on exactly one of
//      the two backends, which is the failure mode phases 6 and 19 both hit.
//
//   3. Qt::NoFocus on every proxy, WHICH IS NARROWER THAN SECTION 19.7 ASKED
//      FOR AND IS DELIBERATE. That plan wanted them in a real tab chain; the
//      first cut built it that way and broke the most-used key in the
//      application -- Qt gave initial focus to the first focusable widget in
//      the window, every other transport widget is NoFocus by an old rule, so
//      the Rewind proxy took it and the first Space on a fresh launch read
//      `speed -2.00x | Reverse Play`. See the note in the .cpp. Screen readers
//      navigate the accessibility tree rather than the tab chain, so the
//      announcement survives; what is given up is Tab-to-activate, and every
//      command here already has a shortcut that works from anywhere.
//
//   4. It still inherits OverlayHooks::holdVisible correctly, for the simple
//      reason that nothing here takes focus at all -- so the transport's
//      auto-hide behaves exactly as it did before the proxies existed. The
//      first cut would have held the panel up whenever a proxy had focus,
//      which is defensible, but it is a behaviour change to phase 6's
//      signed-off auto-hide and was never asked for.
//
// What it does NOT do, stated because it must not be claimed: a magnifier or
// Windows high-contrast mode still cannot affect renderer-drawn art. Section
// 19.7 names that as unresolved and it remains so; the atlas would have to be
// rebuilt on QEvent::ThemeChange.
class OverlayAccessibility final : public QObject {
    Q_OBJECT
public:
    // What each proxy announces itself as, and what it does when activated.
    // Actions rather than callbacks: the name, the shortcut and the enabled
    // state then all come from the one QAction every other surface uses, which
    // is the spec's shared-actions requirement applied to the accessible names
    // as well as to the commands.
    struct Commands {
        QAction* playPause = nullptr;
        QAction* rewind = nullptr;
        QAction* fastForward = nullptr;
        QAction* share = nullptr;
        // UI redesign roadmap step 5's four. Fullscreen and Mute are CHECKABLE,
        // which the proxy already handles: QAction::changed fires for checked
        // as well as for text and enabled, so the name follows a control whose
        // state changes without a second connection.
        QAction* goToStart = nullptr;
        QAction* goToEnd = nullptr;
        QAction* mute = nullptr;
        QAction* loop = nullptr;
        QAction* fullscreen = nullptr;
    };

    // `host` is the viewer widget the overlay is composited onto; the proxies
    // become its children and are positioned in its logical coordinates.
    OverlayAccessibility(QWidget* host, const trace::render::OverlayModel* model,
                         Commands commands);

    // Reposition every proxy from the model's current rects. Cheap -- five
    // setGeometry calls on widgets that never paint -- and idempotent, so it
    // can be called from a resize without a debounce.
    void sync();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    QWidget* host_ = nullptr;
    const trace::render::OverlayModel* model_ = nullptr;
    Commands commands_;
    std::vector<QWidget*> proxies_;
    // The model's layout revision as of the last sync. A paint whose revision
    // has not moved needs no work, so the per-paint cost is one comparison.
    long long lastLayoutRevision_ = -1;
};

} // namespace trace::app
