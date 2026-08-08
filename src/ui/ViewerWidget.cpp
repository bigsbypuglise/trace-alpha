#include "ui/ViewerWidget.h"

#include <QPainter>
#include <QElapsedTimer>
#include <QByteArray>

namespace trace::ui {
namespace {

// Filtering the fit-to-window resample costs real time per paint at 4K.
// TRACE_NEAREST_SCALE=1 restores the old point-sampled blit so the two can be
// A/B'd against the HUD's `draw` figure without a rebuild.
bool nearestScaleForced() {
    static const bool on = [] {
        const QByteArray v = qgetenv("TRACE_NEAREST_SCALE");
        return !v.isEmpty() && v != "0";
    }();
    return on;
}

} // namespace

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
    clock_.start();
}

void ViewerWidget::setFrame(const trace::core::VideoFrame& frame) {
    frame_ = frame;
    // Zero-copy: a read-only QImage view over the frame's buffer, whose cleanup
    // functor keeps the buffer alive for as long as the view exists. Built here
    // rather than in paintEvent so a repaint that is not a new frame (resize,
    // expose, overlay damage) costs nothing extra.
    image_ = frame_.toQImage();
    hasImage_ = !image_.isNull();
    updateRequestedNs_ = clock_.nsecsElapsed();
    ++perfStats_.updateCount;
    update();
}

void ViewerWidget::clearImage() {
    frame_ = trace::core::VideoFrame{};
    image_ = QImage();
    hasImage_ = false;
    update();
}

void ViewerWidget::setCenterText(const QString& text) {
    centerText_ = text;
    update();
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

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

    QElapsedTimer timer;
    timer.start();
    double drawImageMs = 0.0;

    // Inner scope so ~QPainter (which flushes to the backing store) runs
    // before the total is taken; the old timer stopped short of it.
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0));

        if (hasImage_) {
            const QSize fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
            QRect target((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(), fitted.height());
            // Nearest-neighbour point sampling is what jagged every diagonal
            // edge in the frame: any window that is not exactly the source
            // resolution drops whole pixel rows and columns. Filter whenever
            // the frame is being resampled -- but never when it maps 1:1, where
            // filtering could only soften pixels the user is inspecting.
            const bool resampled = fitted != image_.size();
            const bool filtered = resampled && !nearestScaleForced();
            p.setRenderHint(QPainter::SmoothPixmapTransform, filtered);
            perfStats_.lastDrawWasScaled = resampled;
            perfStats_.lastDrawWasFiltered = filtered;
            perfStats_.lastDrawSize = fitted;
            QElapsedTimer drawTimer;
            drawTimer.start();
            p.drawImage(target, image_);
            drawImageMs = static_cast<double>(drawTimer.nsecsElapsed()) / 1'000'000.0;
        } else {
            p.setPen(QColor(150, 150, 150));
            p.drawText(rect(), Qt::AlignCenter, centerText_);
        }

        // Same scope as the original measurement, for continuity.
        const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        perfStats_.lastPaintMs = paintMs;
        ++perfStats_.samples;
        const double n = static_cast<double>(perfStats_.samples);
        perfStats_.avgPaintMs += (paintMs - perfStats_.avgPaintMs) / n;
    }

    const double paintTotalMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    perfStats_.lastPaintTotalMs = paintTotalMs;
    perfStats_.lastDrawImageMs = drawImageMs;
    ++perfStats_.paintCount;
    const double pn = static_cast<double>(perfStats_.paintCount);
    perfStats_.avgPaintTotalMs += (paintTotalMs - perfStats_.avgPaintTotalMs) / pn;
    perfStats_.avgDrawImageMs += (drawImageMs - perfStats_.avgDrawImageMs) / pn;
    perfStats_.avgUpdateToPaintMs += (perfStats_.lastUpdateToPaintMs - perfStats_.avgUpdateToPaintMs) / pn;
}

} // namespace trace::ui
