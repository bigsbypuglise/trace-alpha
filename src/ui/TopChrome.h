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
// neither visible NOR hit-testable, while native siblings are both -- and lose
// translucency, because neither design puts the video pixels anywhere Qt can
// blend against. So the strip is opaque. That is not a shortfall invented here:
// the design package supplies this case's own answer, a solid `#14161A` fallback
// for when Windows transparency effects are off, and the backdrop blur it falls
// back FROM is roadmap step 10, which is flagged as the item with real
// presentation risk against a flip-model swapchain.
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

    QLabel* markLabel_ = nullptr;
    QLabel* wordmarkLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QMenuBar* menuBar_ = nullptr;
    QString mediaTitle_;
};

} // namespace trace::ui
