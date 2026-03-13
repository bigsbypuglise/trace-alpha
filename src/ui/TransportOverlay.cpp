#include "ui/TransportOverlay.h"

#include <QPainter>

namespace trace::ui {

TransportOverlay::TransportOverlay(QWidget* parent) : QWidget(parent) {
    setFixedHeight(42);
}

void TransportOverlay::setTransport(const QString& mode, qint64 frame, double speed, const QString& action) {
    mode_ = mode;
    frame_ = frame;
    speed_ = speed;
    action_ = action;
    update();
}

void TransportOverlay::setHudLine(const QString& line) {
    hudLine_ = line;
    update();
}

void TransportOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(16, 16, 16));
    p.setPen(QColor(220, 220, 220));

    const QString top = QString("  %1 | frame %2 | speed %3x | %4")
                            .arg(mode_)
                            .arg(frame_)
                            .arg(speed_, 0, 'f', 2)
                            .arg(action_);
    p.drawText(rect().adjusted(0, -10, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, top);

    p.setPen(QColor(170, 170, 170));
    p.drawText(rect().adjusted(0, 12, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, "  " + hudLine_);
}

} // namespace trace::ui
