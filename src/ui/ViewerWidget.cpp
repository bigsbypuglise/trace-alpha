#include "ui/ViewerWidget.h"

#include <QPainter>
#include <QElapsedTimer>

namespace trace::ui {

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(640, 360);
}

void ViewerWidget::setImage(const QImage& image) {
    image_ = image;
    hasImage_ = !image_.isNull();
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

    QElapsedTimer timer;
    timer.start();

    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0));

    if (hasImage_) {
        const QSize fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(), fitted.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(target, image_);
    } else {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter, centerText_);
    }

    const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    perfStats_.lastPaintMs = paintMs;
    ++perfStats_.samples;
    const double n = static_cast<double>(perfStats_.samples);
    perfStats_.avgPaintMs += (paintMs - perfStats_.avgPaintMs) / n;
}

} // namespace trace::ui
