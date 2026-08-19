#include "ui/TransportOverlay.h"

#include <QPainter>

namespace trace::ui {

namespace {
constexpr int kTransportRowHeight = 22;
constexpr int kHudRowHeight = 14;
constexpr int kBottomPadding = 6;
} // namespace

TransportOverlay::TransportOverlay(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kTransportRowHeight + kHudRowHeight + kBottomPadding);
}

void TransportOverlay::setTransport(const QString& mode, qint64 frame, double speed, const QString& action) {
    mode_ = mode;
    frame_ = frame;
    speed_ = speed;
    action_ = action;
    update();
}

void TransportOverlay::setHudLine(const QString& line) {
    hudLines_ = line.split('\n', Qt::SkipEmptyParts);
    if (hudLines_.isEmpty()) hudLines_ << QString();
    setFixedHeight(kTransportRowHeight + kHudRowHeight * hudLines_.size() + kBottomPadding);
    update();
}

void TransportOverlay::setStorageState(const QString& state) {
    if (storageState_ == state) return;
    storageState_ = state;
    update();
}

void TransportOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(16, 16, 16));
    // Tabular figures, the same feature the strip's readouts use (owner item
    // 9, 2026-08-18): the frame counter is mid-line here, so every proportional
    // digit change reflowed everything after it. One feature on one font, so
    // the HUD and the strip cannot disagree about how a running number moves.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QFont hudFont = p.font();
    hudFont.setFeature(QFont::Tag("tnum"), 1);
    p.setFont(hudFont);
#endif
    p.setPen(QColor(220, 220, 220));

    const QString top = QString("  %1 | frame %2 | speed %3x | %4")
                            .arg(mode_)
                            .arg(frame_)
                            .arg(speed_, 0, 'f', 2)
                            .arg(action_);
    p.drawText(QRect(0, 0, width(), kTransportRowHeight), Qt::AlignVCenter | Qt::AlignLeft, top);

    // Right-aligned on the transport row. Amber rather than red: waiting on a
    // mounted filespace is a normal condition, not a fault.
    if (!storageState_.isEmpty()) {
        p.setPen(QColor(235, 175, 70));
        p.drawText(QRect(0, 0, width() - 8, kTransportRowHeight),
                   Qt::AlignVCenter | Qt::AlignRight, storageState_);
    }

    p.setPen(QColor(170, 170, 170));
    for (int i = 0; i < hudLines_.size(); ++i) {
        const QRect row(0, kTransportRowHeight + i * kHudRowHeight, width(), kHudRowHeight);
        p.drawText(row, Qt::AlignVCenter | Qt::AlignLeft, "  " + hudLines_.at(i));
    }
}

} // namespace trace::ui
