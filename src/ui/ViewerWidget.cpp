#include "ui/ViewerWidget.h"

#include <QPainter>

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

    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0));

    if (hasImage_) {
        const QSize fitted = image_.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(), fitted.height());
        // Prioritize playback responsiveness for large frames (e.g. 4K ProRes).
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(target, image_);
        return;
    }

    p.setPen(QColor(150, 150, 150));
    p.drawText(rect(), Qt::AlignCenter, centerText_);
}

} // namespace trace::ui
