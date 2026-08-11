#include "ui/ViewerWidget.h"

#include <QDebug>
#include <QMouseEvent>
#include <QResizeEvent>

#include <cmath>

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
    const bool overlayLost = ok && overlayModel_.enabled() && renderer->overlayDrawFailed();
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
    if (a >= 1.0) setMinimumSize(qMax(1, static_cast<int>(std::lround(360.0 * a))), 360);
    else setMinimumSize(360, qMax(1, static_cast<int>(std::lround(360.0 / a))));
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

void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!overlayModel_.enabled()) { QWidget::mouseMoveEvent(event); return; }
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseMove(p.x(), p.y())) QWidget::mouseMoveEvent(event);
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
    if (!overlayModel_.enabled() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint p = toDevice(event->position());
    if (!overlayModel_.onMouseDown(p.x(), p.y())) QWidget::mousePressEvent(event);
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!overlayModel_.enabled() || event->button() != Qt::LeftButton) {
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

void ViewerWidget::setCenterText(const QString& text) {
    if (renderer_) renderer_->setPlaceholderText(text);
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
