#include "ui/ViewerWidget.h"

#include <QDebug>
#include <QResizeEvent>

namespace trace::ui {

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
    clock_.start();

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
    return true;
}

QString ViewerWidget::rendererName() const {
    return renderer_ ? renderer_->name() : QStringLiteral("none");
}

QPaintEngine* ViewerWidget::paintEngine() const {
    return hostHwndSpike_ ? nullptr : QWidget::paintEngine();
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

    const double pn = static_cast<double>(perfStats_.paintCount);
    if (pn > 0.0) {
        perfStats_.avgUpdateToPaintMs +=
            (perfStats_.lastUpdateToPaintMs - perfStats_.avgUpdateToPaintMs) / pn;
    }
}

} // namespace trace::ui
