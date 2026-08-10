#include "ui/ViewerWidget.h"

#include <QDebug>
#include <QMouseEvent>
#include <QResizeEvent>

#include <cmath>

namespace trace::ui {

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
    clock_.start();

    // Read once, here, so both backends get the same answer. It used to be read
    // inside the D3D11 backend, which was defensible while that backend was
    // opt-in and is not now that it is the default: an overlay that appears on
    // the shipping renderer and not on its own control would make every A/B
    // between them a comparison of two different applications.
    overlayModel_.setEnabled(trace::render::OverlayModel::enabledByEnvironment());
    if (overlayModel_.enabled()) {
        // Say so on stderr. The mechanism is real now, but the artwork is still
        // the spike's placeholder geometry until the interface pass draws it,
        // and anyone switching this on should know which half they are looking
        // at.
        qWarning().noquote()
            << "Trace: floating transport overlay enabled "
               "(mechanism is real; artwork is still placeholder).";
        // Pointer motion has to reveal it, and a move with no button held only
        // arrives with tracking on. Harmless under the D3D11 backend, which
        // never receives these events at all.
        setMouseTracking(true);
    }

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

    if (!renderer->initialize(this, error)) {
        // Leave no half-applied native state behind for the fallback to inherit.
        nativeSurface_ = false;
        setAttribute(Qt::WA_NoSystemBackground, false);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        return false;
    }

    renderer_ = std::move(renderer);
    // After initialize(), because a backend that failed has already been
    // discarded and must not be left holding a pointer to the model. Handed to
    // both backends unconditionally: whether anything is drawn is the model's
    // enabled() to decide, not the renderer's.
    renderer_->setOverlay(&overlayModel_);
    return true;
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
