#include "ui/ViewerWidget.h"

#include <QDebug>
#include <QMouseEvent>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace trace::ui {

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    applyMinimumForAspect();
    clock_.start();

    // Read once, here, so both backends get the same answer. It used to be read
    // inside the D3D11 backend, which was defensible while that backend was
    // opt-in and is not now that it is the default: an overlay that appears on
    // the shipping renderer and not on its own control would make every A/B
    // between them a comparison of two different applications.
    overlayModel_.setEnabled(trace::render::OverlayModel::enabledByEnvironment());
    if (overlayModel_.enabled()) {
        // Pointer motion has to reveal it, and a move with no button held only
        // arrives with tracking on. Harmless under the D3D11 backend, which
        // never receives these events at all.
        setMouseTracking(true);
    } else {
        // Announce the NON-default now that the floating transport is the
        // default (spec phase 6). The warning used to say the overlay was on
        // and its artwork placeholder; both halves of that expired in the same
        // commit, and a stale announcement is worse than none.
        qWarning().noquote()
            << "Trace: floating transport overlay disabled - using the docked "
               "transport bar (TRACE_TRANSPORT_BAR).";
    }

    // The view transform starts at identity and is driven by the Edit menu's
    // actions from spec phase 10. TRACE_VIEW_TRANSFORM was the interim knob
    // that stood in for them and it left with the phase that made it redundant,
    // the way TRACE_SHUTTLE_ENTRY left with phase 5.

    renderer_ = trace::render::createRenderer();
    QString error;
    if (!adoptRenderer(std::move(renderer_), error)) {
        // A GPU backend can fail for reasons only visible once there is a
        // device and a window -- no adapter, a driver that will not create a
        // swapchain for this HWND. That is not a reason to run with no picture:
        // fall back to the CPU backend, which cannot fail, and say loudly which
        // one is actually presenting.
        qWarning().noquote() << "Trace: renderer failed to initialize:" << error
                             << "- falling back to cpu.";
        rendererFellBack_ = true;
        QString cpuError;
        if (!adoptRenderer(trace::render::createCpuRenderer(), cpuError)) {
            qWarning().noquote() << "Trace: cpu renderer failed to initialize:" << cpuError;
        }
    }
}

// Installs a renderer and applies the widget-level contract it asks for. The
// native-surface attributes must be set BEFORE initialize(), because that is
// what realises the HWND the backend takes -- so this cannot be split into
// "create, configure, initialize" at the call site without the order becoming a
// convention rather than a property.
bool ViewerWidget::adoptRenderer(std::unique_ptr<trace::render::VideoRenderer> renderer,
                                 QString& error) {
    if (!renderer) { error = QStringLiteral("no renderer"); return false; }

    nativeSurface_ = renderer->usesNativeSurface();
    // The backend needs a real HWND to parent its surface to, and Qt must stop
    // erasing a rect it no longer owns a single pixel of.
    //
    // Note what is deliberately NOT here: WA_PaintOnScreen and a null
    // paintEngine(). That recipe was measured and does work on Windows, but it
    // is documented X11-only, and the backend owns a child window instead --
    // see createSurfaceWindow() for why that was preferred. The consequence
    // here is the good one: from Qt's point of view this stays an ordinary
    // widget whose paintEvent happens to draw nothing.
    setAttribute(Qt::WA_NativeWindow, nativeSurface_);
    setAttribute(Qt::WA_NoSystemBackground, nativeSurface_);
    setAttribute(Qt::WA_OpaquePaintEvent, nativeSurface_);
    hostHwndSpike_ = nativeSurface_ && !qgetenv("TRACE_D3D11_HOSTHWND").isEmpty();
    setAttribute(Qt::WA_PaintOnScreen, hostHwndSpike_);

    const bool ok = renderer->initialize(this, error);
    // A backend that came up but cannot draw the overlay is a failure now that
    // the overlay is the only transport (spec phase 6). It was survivable while
    // the overlay was an off-by-default spike; today it would leave the window
    // with a picture and no controls, which is exactly the silent degradation
    // the renderer name in the HUD exists to make visible.
    // NOT gated on overlayModel_.enabled() any more. It was, because the only
    // thing the composited path carried was the floating transport, and with
    // TRACE_TRANSPORT_BAR=1 there were real widgets doing that job instead.
    // Roadmap step 3 put the EMPTY STATE on the same quads, and that is drawn
    // in both transport modes -- so a backend that cannot draw quads can no
    // longer present a window with no media in it, whichever transport is
    // selected, and that is a fallback rather than something to survive.
    const bool overlayLost = ok && renderer->overlayDrawFailed();
    if (overlayLost) error = QStringLiteral("backend cannot draw the floating transport");
    if (!ok || overlayLost) {
        // Leave no half-applied native state behind for the fallback to inherit.
        nativeSurface_ = false;
        hostHwndSpike_ = false;
        setAttribute(Qt::WA_NoSystemBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setAttribute(Qt::WA_PaintOnScreen, false);
        return false;
    }

    renderer_ = std::move(renderer);
    // After initialize(), because a backend that failed has already been
    // discarded and must not be left holding a pointer to the model. Handed to
    // both backends unconditionally: whether anything is drawn is the model's
    // enabled() to decide, not the renderer's.
    renderer_->setOverlay(&overlayModel_);
    // Through applySourceShape rather than setViewTransform directly, so a
    // backend adopted after media was already open inherits the container's
    // rotation and pixel aspect too. Handing it the bare user transform here is
    // how a fallback to cpu would silently un-rotate a phone clip.
    applySourceShape();
    return true;
}

void ViewerWidget::setViewTransform(const trace::render::ViewTransform& transform) {
    if (transform == viewTransform_) return;
    viewTransform_ = transform;
    applySourceShape();
    update();
}

void ViewerWidget::setSourceShape(double pixelAspect, int rotationDegrees) {
    const double par = (pixelAspect > 0.0) ? pixelAspect : 1.0;
    const int rot = ((rotationDegrees % 360) + 360) % 360;
    if (par == sourcePixelAspect_ && rot == sourceRotationDegrees_) return;
    sourcePixelAspect_ = par;
    sourceRotationDegrees_ = rot;
    applySourceShape();
    update();
}

// THE ONE PLACE THE CONTAINER'S ROTATION AND THE USER'S ARE COMBINED.
//
// A file that carries rotation metadata and a user who pressed Rotate Right are
// asking for the same operation for different reasons, and the renderer should
// only ever be told the answer. Keeping them separate up to this point is what
// makes Reset View Transform mean "back to how the file says it should look"
// rather than "back to un-rotated", which on a phone clip would be wrong.
//
// Rotations commute, so the composition is an addition; the user's flips stay
// as they are, because they are already expressed in screen space and the
// container states no flip. rotatedOnScreen()'s mirror compensation is
// unaffected by a constant offset -- it decides the SIGN of the user's step,
// and the step is the same whatever constant is added afterwards.
void ViewerWidget::applySourceShape() {
    if (!renderer_) return;
    trace::render::ViewTransform composed = viewTransform_;
    composed.quarterTurns =
        ((viewTransform_.quarterTurns + sourceRotationDegrees_ / 90) % 4 + 4) % 4;
    renderer_->setViewTransform(composed);
    renderer_->setPixelAspect(sourcePixelAspect_);
    // The scale's reference is this same composition, so it is re-pushed from
    // here rather than left to whoever changed the transform to remember. A
    // rotation applied while zoomed to 2:1 must keep 2:1 and change what 2:1 is
    // measured against; those are the same statement and this is the only place
    // that can make them one call.
    applyViewScale();
}

// Stored samples -> what those samples occupy on screen at 1:1: the pixel
// aspect, then the composed rotation. The same two operations, in the same
// order, that displayedAspect() takes the ratio of and that both backends apply
// to the frame -- expressed once here so the scale and the aspect cannot come
// to different conclusions about the shape of the same file.
QSize ViewerWidget::displayedSourceSize() const {
    if (sourcePixels_.isEmpty()) return QSize();
    return trace::render::ViewTransform::rotated(
               viewTransform_.quarterTurns + sourceRotationDegrees_ / 90)
        .apply(trace::render::applyPixelAspect(sourcePixels_, sourcePixelAspect_));
}

void ViewerWidget::applyViewScale() {
    if (!renderer_) return;
    viewScale_.referenceDisplayed = displayedSourceSize();
    renderer_->setViewScale(viewScale_);
}

void ViewerWidget::setSourcePixelSize(QSize pixels) {
    if (pixels == sourcePixels_) return;
    sourcePixels_ = pixels;
    // NOT a reset to fit. Opening media resets the scale, and MainWindow does
    // that explicitly at the same point it resets the transform -- putting it
    // here as well would make "the source size was re-stated" and "a new file
    // was opened" the same event, and they are not: a rotation restates the
    // first without being the second.
    applyViewScale();
    // repaint(), NOT update(), and it is phase 10's finding applied to the
    // second thing that has it. The fit and the reduction taps are measured BY
    // the paint and reported after it, so refreshing the HUD after a merely
    // SCHEDULED repaint prints the previous scale's `display` -- and on a
    // paused file nothing refreshes it again, so it stays wrong. Measured
    // before the fix: Ctrl+0 on 4K media visibly magnified the picture while
    // `display` still read the fit's `1201x676 filtered x2`, which is exactly
    // what a build whose zoom had failed to reach the renderer would print.
    repaint();
}

double ViewerWidget::fitScale() const {
    const QSize ref = displayedSourceSize();
    if (ref.isEmpty()) return 1.0;
    const QSize host = trace::render::hostDeviceSize(this);
    return std::min(static_cast<double>(host.width()) / ref.width(),
                    static_cast<double>(host.height()) / ref.height());
}

double ViewerWidget::currentScale() const {
    if (viewScale_.fitToWindow) return fitScale();
    return std::min(viewScale_.scale, trace::render::maxViewScale(displayedSourceSize()));
}

void ViewerWidget::setFitToWindow() {
    if (viewScale_.fitToWindow) return;
    viewScale_.fitToWindow = true;
    // The pan goes with it. It is meaningless at fit -- the picture is inside
    // the viewport on both axes, so clampPan would zero it anyway -- and
    // leaving a value behind would make a later Actual Size land somewhere the
    // user last dragged to rather than on the middle of the picture.
    viewScale_.pan = QPointF();
    applyViewScale();
    // repaint(), for the reason given at setFitToWindow().
    repaint();
}

void ViewerWidget::setActualSize() {
    setScaleAnchored(1.0);
}

// The ladder, and it is anchored on 1:1 by DOUBLING for a reason that outlives
// taste: at an integer power of two, point sampling replicates each source
// sample into an exact square block. A ladder of 1.25 steps would magnify by a
// non-integer factor, and nearest-neighbour at 3.2:1 draws some samples three
// device pixels wide and others four -- visibly uneven, on the one path whose
// whole purpose is showing the samples faithfully.
void ViewerWidget::zoomStep(int direction) {
    if (direction == 0 || sourcePixels_.isEmpty()) return;
    static constexpr double kLadder[] = {0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    const double maxScale = trace::render::maxViewScale(displayedSourceSize());
    const double now = currentScale();

    double target = 0.0;
    if (direction > 0) {
        // The first rung strictly above where the picture is. From fit that is
        // "the next rung past this arbitrary ratio", which is what makes one
        // press from fit always visibly zoom in rather than sometimes landing
        // on the ratio it was already at.
        for (const double rung : kLadder) {
            if (rung > now * 1.0001 && rung <= maxScale) { target = rung; break; }
        }
        // Already at or past the top rung this media can reach. Pin to the cap
        // rather than doing nothing, so the command is not silently dead on a
        // plate whose ladder is short.
        if (target == 0.0) target = std::min(maxScale, kLadder[std::size(kLadder) - 1]);
    } else {
        for (auto it = std::rbegin(kLadder); it != std::rend(kLadder); ++it) {
            if (*it < now * 0.9999) { target = *it; break; }
        }
        if (target == 0.0) target = kLadder[0];
    }
    setScaleAnchored(target);
}

// Zoom keeps the CENTRE OF THE VIEWPORT on the same point of the picture. The
// destination rect is the viewport's centre plus the pan, so the source point
// under the centre stays put exactly when the pan scales with the picture --
// without this, zooming in while panned to a corner walks the picture back
// toward the middle, one rung at a time.
void ViewerWidget::setScaleAnchored(double scale) {
    if (!(scale > 0.0) || sourcePixels_.isEmpty()) return;
    const double from = currentScale();
    const double to = std::min(scale, trace::render::maxViewScale(displayedSourceSize()));
    if (from > 0.0 && !viewScale_.fitToWindow) viewScale_.pan *= (to / from);
    viewScale_.fitToWindow = false;
    viewScale_.scale = to;
    viewScale_.pan = trace::render::clampPan(pictureDeviceSize(),
                                             trace::render::hostDeviceSize(this),
                                             viewScale_.pan);
    applyViewScale();
    // repaint(), for the reason given at setFitToWindow().
    repaint();
}

// The picture's size on screen at the scale in force -- the same product
// viewDeviceRect computes, and the reason a pan can be clamped here without the
// host learning how the renderer lays a rect out.
QSize ViewerWidget::pictureDeviceSize() const {
    const QSize ref = displayedSourceSize();
    if (ref.isEmpty()) return QSize();
    const double s = currentScale();
    return QSize(std::max(1, static_cast<int>(std::lround(ref.width() * s))),
                 std::max(1, static_cast<int>(std::lround(ref.height() * s))));
}

bool ViewerWidget::canPan() const {
    if (viewScale_.fitToWindow) return false;
    const QSize picture = pictureDeviceSize();
    if (picture.isEmpty()) return false;
    const QSize host = trace::render::hostDeviceSize(this);
    return picture.width() > host.width() || picture.height() > host.height();
}

void ViewerWidget::panBy(QPointF deltaDevice) {
    if (!canPan()) return;
    // Clamped on every step rather than only at the draw, so holding the
    // pointer against the edge of the screen cannot accumulate an offset that
    // then has to be dragged back through before the picture moves.
    viewScale_.pan = trace::render::clampPan(pictureDeviceSize(),
                                             trace::render::hostDeviceSize(this),
                                             viewScale_.pan + deltaDevice);
    applyViewScale();
    // repaint(), for the reason given at setFitToWindow().
    repaint();
}

// A FIXED 640x360 FLOOR IS ITSELF A 16:9 ASSUMPTION, and it fights the aspect
// lock on every other shape: a 1x1 clip could not go below 640x640, a 4x5 below
// 640x800, and a 9:16 below 640x1138 -- taller than many work areas at 125% DPI,
// which would make the lock silently unsatisfiable on exactly the assets phase
// 10 used.
//
// The rule that replaces it keeps the INTENT of the old number rather than the
// number: 360 logical px on the SHORTER displayed axis. At 16:9 that is
// 640x360 to the pixel, so nothing about startup geometry moves for 16:9 media
// and every recorded `win` figure stays comparable; at 1:1 it is 360x360, at
// 4:5 360x450, at 9:16 360x640 -- all smaller than the old floor rather than
// larger, so no shape becomes harder to make small than it was.
void ViewerWidget::applyMinimumForAspect() {
    const double a = minimumAspect_;
    if (!(a > 0.0)) { setMinimumSize(640, 360); return; }
    int w = 0;
    int h = 0;
    if (a >= 1.0) { w = static_cast<int>(std::lround(360.0 * a)); h = 360; }
    else { w = 360; h = static_cast<int>(std::lround(360.0 / a)); }
    // AND THE TRANSPORT HAS TO FIT (owner, 2026-08-11). The floating panel is a
    // settled 460 logical px (spec phase 6 signed that number off), so a window
    // narrower than it has a transport hanging off both sides. On a tall clip
    // this is the binding constraint rather than the 360px short axis -- a 9:16
    // floor of 360 wide becomes 460 -- and the height is recomputed from it so
    // widening the floor does not make the floor the wrong shape.
    if (w < kMinTransportWidth) {
        w = kMinTransportWidth;
        h = qMax(1, static_cast<int>(std::lround(w / a)));
    }
    setMinimumSize(qMax(1, w), qMax(1, h));
}

void ViewerWidget::setMinimumAspect(double aspect) {
    if (!(aspect > 0.0) || aspect == minimumAspect_) return;
    minimumAspect_ = aspect;
    applyMinimumForAspect();
}

double ViewerWidget::displayedAspect(QSize sourcePixels) const {
    if (sourcePixels.isEmpty()) return 0.0;
    const QSize shown = trace::render::ViewTransform::rotated(
                            viewTransform_.quarterTurns + sourceRotationDegrees_ / 90)
                            .apply(trace::render::applyPixelAspect(sourcePixels, sourcePixelAspect_));
    if (shown.isEmpty()) return 0.0;
    return static_cast<double>(shown.width()) / static_cast<double>(shown.height());
}

// Logical -> device. The model lays out in device pixels because the D3D11
// surface has no other space, so this is where the CPU path joins it.
QPoint ViewerWidget::toDevice(const QPointF& logical) const {
    const double dpr = devicePixelRatioF();
    return QPoint(static_cast<int>(std::lround(logical.x() * dpr)),
                  static_cast<int>(std::lround(logical.y() * dpr)));
}

QString ViewerWidget::rendererName() const {
    return renderer_ ? renderer_->name() : QStringLiteral("none");
}

bool ViewerWidget::rendererAcceptsPlanarYuv() const {
    // Asked of whatever was ACTUALLY adopted, which after a failed GPU
    // initialize is the CPU backend. A cached answer taken from the requested
    // backend would tell the decoder to skip swscale for a renderer that needs
    // BGRA, and every frame would come out blank.
    return renderer_ && renderer_->acceptsPlanarYuv();
}

QPaintEngine* ViewerWidget::paintEngine() const {
    return hostHwndSpike_ ? nullptr : QWidget::paintEngine();
}

void ViewerWidget::setCursorHidden(bool hidden) {
    if (hidden) setCursor(Qt::BlankCursor);
    else unsetCursor();
    if (renderer_) renderer_->setCursorHidden(hidden);
}

void ViewerWidget::setOverlayHooks(const trace::render::OverlayHooks& hooks) {
    // The hooks belong to the model, not to a backend. That is the whole point
    // of the split: the commands are the application's, the geometry is the
    // model's, and a renderer only puts rectangles on a screen.
    overlayModel_.setHooks(hooks);
}

// NO LONGER GATED ON overlayEnabled(), and that is spec phase 15 rather than a
// tidy-up. The model routes a press that lands on no control to the pan
// gesture, which is a VIEW gesture and must survive TRACE_TRANSPORT_BAR=1 --
// the same reason mouseDoubleClickEvent below has never been gated. The model
// still checks enabled_ for everything that belongs to the transport, so with
// the docked bar selected these reach the pan branch and nothing else.
void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseMove(p.x(), p.y())) QWidget::mouseMoveEvent(event);
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseDown(p.x(), p.y())) QWidget::mousePressEvent(event);
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseUp(p.x(), p.y())) QWidget::mouseReleaseEvent(event);
}

// Not gated on overlayEnabled(): the model routes this whether or not the
// floating transport is switched on, because it is a window gesture.
void ViewerWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseDoubleClick(p.x(), p.y())) QWidget::mouseDoubleClickEvent(event);
}

void ViewerWidget::leaveEvent(QEvent* event) {
    if (overlayModel_.enabled()) overlayModel_.onMouseLeave();
    QWidget::leaveEvent(event);
}

void ViewerWidget::setFrame(const trace::core::VideoFrame& frame) {
    frame_ = frame;
    if (renderer_) renderer_->setFrame(frame);
    // Timestamp the repaint request so the queued update()->paintEvent latency
    // can be separated from paint cost itself.
    updateRequestedNs_ = clock_.nsecsElapsed();
    ++perfStats_.updateCount;
    update();
}

void ViewerWidget::clearImage() {
    frame_ = trace::core::VideoFrame{};
    if (renderer_) renderer_->clearFrame();
    update();
}

void ViewerWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (renderer_) renderer_->resize(event->size());
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!renderer_) return;

    const qint64 paintEntryNs = clock_.nsecsElapsed();
    if (updateRequestedNs_ >= 0) {
        perfStats_.lastUpdateToPaintMs =
            static_cast<double>(paintEntryNs - updateRequestedNs_) / 1'000'000.0;
        updateRequestedNs_ = -1;
    } else {
        // Repaint not driven by a new frame (resize, expose, overlay damage).
        perfStats_.lastUpdateToPaintMs = 0.0;
        ++perfStats_.paintsWithoutNewImage;
    }

    // Pushed per paint, beside the dpr the backends push for the same reason:
    // the scale factor can change without a resize, and a stale device-pixel
    // inset would move the empty state's mark rather than merely be out of date.
    overlayModel_.setTopInset(chromeTopInsetLogical_ * devicePixelRatioF());

    renderer_->paint(this);

    // Fold the backend's numbers into the widget's, so the HUD reads one struct
    // and does not have to know which renderer produced them.
    const auto& rs = renderer_->stats();
    perfStats_.lastPaintMs = rs.lastPaintMs;
    perfStats_.avgPaintMs = rs.avgPaintMs;
    perfStats_.samples = rs.samples;
    perfStats_.lastDrawImageMs = rs.lastDrawImageMs;
    perfStats_.avgDrawImageMs = rs.avgDrawImageMs;
    perfStats_.lastPaintTotalMs = rs.lastPaintTotalMs;
    perfStats_.avgPaintTotalMs = rs.avgPaintTotalMs;
    perfStats_.paintCount = rs.paintCount;
    perfStats_.lastDrawWasScaled = rs.lastDrawWasScaled;
    perfStats_.lastDrawWasFiltered = rs.lastDrawWasFiltered;
    perfStats_.lastDrawSize = rs.lastDrawSize;
    perfStats_.lastUploadMs = rs.lastUploadMs;
    perfStats_.avgUploadMs = rs.avgUploadMs;
    perfStats_.uploadCount = rs.uploadCount;
    perfStats_.textureCreates = rs.textureCreates;
    perfStats_.reduceTaps = rs.reduceTaps;

    const double pn = static_cast<double>(perfStats_.paintCount);
    if (pn > 0.0) {
        perfStats_.avgUpdateToPaintMs +=
            (perfStats_.lastUpdateToPaintMs - perfStats_.avgUpdateToPaintMs) / pn;
    }
}

} // namespace trace::ui
