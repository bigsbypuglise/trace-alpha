#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QMenuBar;

namespace trace::ui {

// THE TRANSIENT TOP CHROME (UI redesign roadmap step 7), and the thing to
// understand first is what it is NOT.
//
// It is not a second transport, it is not a second auto-hide, and it is not a
// reimplementation of the menu bar. It is one widget that floats over the top of
// the video, holds the REAL QMenuBar, and is shown and hidden by the SAME reveal
// state OverlayModel already keeps for the floating transport. Top and bottom
// are one system: one idle timer, one hold list, one answer to "is the interface
// out of the way".
//
// WHY THE MENU BAR STAYS A REAL WIDGET. The composited overlay has no widget
// tree, which is why spec phase 14 had to hand-build a UI Automation proxy for
// five transport controls after phase 6 made the overlay the only transport.
// The menu bar is the application's largest accessible surface -- five menus and
// every command in the product -- and it is exposed to a screen reader for free
// only for as long as it is a QMenuBar. Drawing it as quads would repeat phase
// 14 at a much larger scale, and the proxies would have to carry roles, states,
// submenu structure and keyboard navigation that Qt already implements.
//
// WHY IT IS NATIVE, AND WHAT THAT COSTS. Plan section 18.4 measured this
// exactly: over the shipped child-HWND video surface, ordinary Qt children are
// neither visible NOR hit-testable, while native siblings are both -- and Qt
// cannot blend them, because neither design puts the video pixels anywhere Qt
// can reach. What section 18.4 could not rule out was DWM doing the blending
// instead, and owner item 11 proved it does: a WS_EX_LAYERED uniform alpha on
// this window composites against the real video on the d3d11 default (measured
// MAE 0.16 against the per-pixel blend prediction). THE RESTING TRANSLUCENCY
// RIDES ON THAT MECHANISM -- see setFadeOpacity, whose ramp tops out at
// kRestingAlpha rather than opaque (owner item 8, option B, 2026-08-19).
//
// THE TWO BACKENDS DELIBERATELY DIFFER HERE, BY OWNER DECISION (2026-08-19).
// TRACE_RENDERER=cpu ignores the layered alpha for a known reason -- Qt's
// native children share the top-level backing store, so the rows under the
// strip hold a baked copy of the strip itself and strip-over-strip reads
// opaque -- and the owner accepted the divergence rather than fund the
// composited-quads rebuild. Any cross-backend comparison of the strip band
// OVER VIDEO now measures this decision, not a defect; the empty state stays
// comparable because the strip is held opaque there (see MainWindow's hook).
//
// The design package's solid `#14161A` "fallback when transparency effects are
// disabled in Windows Settings" is still honoured literally: with the setting
// off, the resting alpha is opaque and the fallback is exactly what shows.
//
// ITS PARENT IS THE CENTRAL WIDGET AND NOT THE VIEWER, AND THAT IS A MEASURED
// REQUIREMENT. A native child of the VIEWER is a sibling of the D3D11 surface
// window, and in that position it corrupts the surface's own overlay pass: the
// transport panel's first quad draws correctly and every quad after it renders
// as if its sampled colour were zero -- black glyph cells, a black timeline
// track -- on the default renderer, while TRACE_RENDERER=cpu is perfect.
//
// It is not a data fault, and that was established rather than assumed. The
// atlas texture was read back from the GPU into a STAGING copy and compared
// against the QImage it was uploaded from: byte-identical at rows above and
// below the fault, including an opaque white glyph texel where black was drawn.
// The uv rects, alpha and brighten were printed per quad and are all correct.
// So the inputs are right and the device produces the wrong pixels; the cause
// is left unattributed rather than guessed at.
//
// One level up in the HWND tree the fault is gone completely -- same widget,
// same attributes, same drawing, and the transport renders exactly as it does
// with no chrome at all. The strip still covers the top of the picture, because
// the viewer is the full width of the central widget and sits at its top. This
// is a THIRD row for section 18.4's table, and the operative distinction is not
// "native or not" but WHERE in the window tree the native window sits relative
// to the swapchain's own.
//
// It is native on BOTH backends deliberately. A translucent strip on
// TRACE_RENDERER=cpu and an opaque one on the d3d11 default would be exactly the
// kind of divergence between the shipping path and its own control that the
// overlay model exists to prevent, and it would put a real difference inside
// every cross-backend pixel comparison.
class TopChrome final : public QWidget {
    Q_OBJECT
public:
    explicit TopChrome(QWidget* parent);

    // The real menu bar, owned by this widget and parented into it. Created
    // here rather than handed in, so there is exactly one place that decides a
    // QMenuBar not in QMainWindow's menu slot still belongs to this window.
    QMenuBar* menuBar() const { return menuBar_; }

    // What the centre of the strip reads. Empty means no media, which the
    // design's own empty-state mockup shows as a dimmed "No media" rather than
    // as a blank -- the strip says what state the window is in either way.
    void setMediaTitle(const QString& title);

    // Shown or hidden. Called from MainWindow's setChromeRevealed hook, which is
    // driven by OverlayModel's own reveal state, so this never decides for
    // itself when to appear.
    void setRevealed(bool revealed);

    // The strip's height in logical pixels. The design's 38px, and the one place
    // it is written down -- MainWindow positions the strip from this rather than
    // repeating the number.
    static int stripHeightLogical();

    // Owner item 11 (fade) plus owner item 8 option B (resting translucency),
    // one mechanism: WS_EX_LAYERED plus SetLayeredWindowAttributes(LWA_ALPHA)
    // on this widget's own HWND. Driven from OverlayModel's fade opacity
    // through MainWindow, so the strip ramps in lockstep with the composited
    // transport: same kFadeMs, same clock, no second timer.
    //
    // THE RAMP'S CEILING IS kRestingAlpha, NOT OPAQUE (owner decision,
    // 2026-08-19): opacity 1.0 maps to the resting alpha, so the settled strip
    // shows the real video through itself on the d3d11 default. The ceiling is
    // opaque instead when Windows' transparency setting is off -- the design
    // package's own fallback case -- and TRACE_TOPCHROME_ALPHA=N pins the
    // alpha outright for measurement (a mid-fade state as a stable state, or
    // =255 to force the opaque resting strip).
    //
    // Uniform alpha only, never UpdateLayeredWindow -- the paint path is
    // untouched and the alpha is applied by the compositor. A layered child
    // blends against what is beneath it in the window tree, and item 11
    // measured that to be THE VIDEO on d3d11, not black.
    //
    // TRACE_TOPCHROME_FADE=0 is the full rollback: the style is never applied,
    // setRevealed pops, and the strip rests opaque.
    void setFadeOpacity(double opacity01);
    static bool fadeEnabled();

    // Re-reads Windows' transparency setting and re-applies the resting alpha
    // if the effective answer moved. Driven from MainWindow's WM_SETTINGCHANGE
    // handler -- that setting, unlike the environment overrides beside it, can
    // be flipped while Trace is running, and a strip still translucent after
    // the user turned system transparency off is the setting not being
    // honoured. Inherited from the strip backdrop, whose removal this survived:
    // the setting used to gate the painted blur and now gates the resting
    // alpha, which is the same promise kept by a different mechanism.
    void onSystemAppearanceChanged();

    // What the strip's resting alpha is ACTUALLY doing, for the dev HUD:
    // "a215", "opaque (windows)", "a128 (env)" or "opaque (fade off)".
    // Reported because the answer is not decided by the launch -- a machine
    // with Windows transparency off rests opaque while the command line says
    // nothing, which is the silent-degradation class `renderer`, `planar` and
    // `font` are reported for.
    QString stripStateLabel() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Follows the parent's geometry. Installed on the parent rather than driven
    // from MainWindow::resizeEvent because the strip spans the VIEWER, and the
    // viewer's width is not the window's -- and because a widget that keeps its
    // own geometry cannot be left behind by a resize path nobody remembered.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void relayout();
    void applyLayeredAlpha(int alpha);
    // The alpha the current fade opacity maps to under the resting ceiling and
    // any pin -- one expression, so setFadeOpacity and the settings-change
    // reapply cannot disagree about what the strip should read.
    int effectiveAlpha() const;

    // The last alpha applied. Starts at 0 so the first fade-in maps the window
    // transparent and ramps, rather than popping opaque for one tick.
    int fadeAlpha_ = 0;
    // The model's fade opacity as last delivered, kept so a transparency-
    // setting change can recompute the alpha without waiting for the next
    // animation tick -- on a settled strip there is no next tick.
    double fadeOpacity01_ = 0.0;

    QLabel* titleLabel_ = nullptr;
    QMenuBar* menuBar_ = nullptr;
    QString mediaTitle_;
};

} // namespace trace::ui
