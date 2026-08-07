#include "ui/ViewerWidget.h"

#include <QPainter>
#include <QElapsedTimer>

namespace trace::ui {

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
    clock_.start();
}

void ViewerWidget::setImage(const QImage& image) {
    image_ = image;
    hasImage_ = !image_.isNull();
    // Shallow assignment above; timestamp the repaint request so the queued
    // update()->paintEvent latency can be separated from paint cost itself.
    updateRequestedNs_ = clock_.nsecsElapsed();
    ++perfStats_.updateCount;
    update();
}

void ViewerWidget::clearImage() {
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
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
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
