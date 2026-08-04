#include "ui/TransportBar.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QFontDatabase>

namespace trace::ui {
namespace {

// Design tokens (assets/Interface/export/player-icons/README.txt).
constexpr int kHit = 44;
constexpr int kHitRadius = 10;
constexpr int kCenter = 60;
constexpr int kCenterRadius = 15;
constexpr int kMark = 24;        // icon mark inside a standard control
constexpr int kCenterMark = 26;  // icon mark inside the center control

constexpr double kOpacityRest = 0.78;
constexpr double kOpacityFull = 1.00;
constexpr double kOpacityDisabled = 0.26;

constexpr double kBgHover = 0.08;
constexpr double kBgPressed = 0.15;
constexpr double kBgActive = 0.12;
constexpr double kStrokeActive = 0.22;

constexpr int kBarHeight = 76;

} // namespace

TransportButton::TransportButton(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover, true);
    // Keyboard belongs to frame stepping and J-K-L. A transport widget that
    // takes focus would swallow arrows and space.
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kHit, kHit);
}

void TransportButton::setIcon(const QIcon& icon) {
    icon_ = icon;
    update();
}

void TransportButton::setCenterControl(bool center) {
    center_ = center;
    setFixedSize(center ? kCenter : kHit, center ? kCenter : kHit);
    update();
}

void TransportButton::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    update();
}

void TransportButton::setEnabledControl(bool enabled) {
    if (isEnabled() == enabled) return;
    setEnabled(enabled);
    if (!enabled) {
        hovered_ = false;
        pressed_ = false;
    }
    update();
}

QSize TransportButton::sizeHint() const {
    return center_ ? QSize(kCenter, kCenter) : QSize(kHit, kHit);
}

void TransportButton::enterEvent(QEnterEvent* event) {
    Q_UNUSED(event);
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
}

void TransportButton::leaveEvent(QEvent* event) {
    Q_UNUSED(event);
    hovered_ = false;
    pressed_ = false;
    update();
}

void TransportButton::mousePressEvent(QMouseEvent* event) {
    if (!isEnabled() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    pressed_ = true;
    update();
}

void TransportButton::mouseReleaseEvent(QMouseEvent* event) {
    if (!isEnabled() || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasPressed = pressed_;
    pressed_ = false;
    update();
    if (wasPressed && rect().contains(event->position().toPoint())) {
        emit clicked();
    }
}

void TransportButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int radius = center_ ? kCenterRadius : kHitRadius;
    // Half-pixel inset keeps the 1px stroke crisp; must be QRectF, since
    // QRect::adjusted would truncate the 0.5 to 0.
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    double bgAlpha = 0.0;
    double iconOpacity = kOpacityRest;

    if (!isEnabled()) {
        iconOpacity = kOpacityDisabled;
    } else if (pressed_) {
        bgAlpha = kBgPressed;
        iconOpacity = kOpacityFull;
    } else if (hovered_) {
        bgAlpha = kBgHover;
        iconOpacity = kOpacityFull;
    } else if (active_) {
        bgAlpha = kBgActive;
        iconOpacity = kOpacityFull;
    }

    // The center control keeps a resting surface so it reads as primary.
    if (center_ && bgAlpha <= 0.0) {
        bgAlpha = kBgHover;
    }

    if (bgAlpha > 0.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, static_cast<int>(bgAlpha * 255.0)));
        p.drawRoundedRect(box, radius, radius);
    }

    if (active_ && isEnabled()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, static_cast<int>(kStrokeActive * 255.0)), 1.0));
        p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), radius - 1, radius - 1);
    }

    if (icon_.isNull()) return;

    const int mark = center_ ? kCenterMark : kMark;
    const QRect markRect(
        (width() - mark) / 2,
        (height() - mark) / 2,
        mark,
        mark);

    p.setOpacity(iconOpacity);
    // Request the pixmap at device resolution so QIcon picks the 2x/3x file
    // on high-DPI displays instead of upscaling the 24px one. The
    // QIcon::pixmap(size, dpr, ...) overload is Qt 6.8+; CI builds 6.7.2.
    const qreal dpr = devicePixelRatioF();
    QPixmap pm = icon_.pixmap(QSize(mark, mark) * dpr);
    pm.setDevicePixelRatio(dpr);
    p.drawPixmap(markRect, pm);
}

QIcon TransportBar::loadIcon(const QString& baseName) {
    QIcon icon;
    icon.addFile(QStringLiteral(":/ui/%1-24.png").arg(baseName), QSize(24, 24));
    icon.addFile(QStringLiteral(":/ui/%1-48.png").arg(baseName), QSize(48, 48));
    icon.addFile(QStringLiteral(":/ui/%1-72.png").arg(baseName), QSize(72, 72));
    return icon;
}

TransportBar::TransportBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kBarHeight);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);

    playIcon_ = loadIcon(QStringLiteral("play"));
    pauseIcon_ = loadIcon(QStringLiteral("pause"));
    fullscreenIcon_ = loadIcon(QStringLiteral("fullscreen"));
    exitFullscreenIcon_ = loadIcon(QStringLiteral("exit-fullscreen"));

    prevFrameBtn_ = new TransportButton(this);
    prevFrameBtn_->setIcon(loadIcon(QStringLiteral("prev-frame")));
    prevFrameBtn_->setToolTip(tr("Previous frame (Left arrow)"));

    playPauseBtn_ = new TransportButton(this);
    playPauseBtn_->setCenterControl(true);
    playPauseBtn_->setIcon(playIcon_);
    playPauseBtn_->setToolTip(tr("Play / Pause (Space)"));

    nextFrameBtn_ = new TransportButton(this);
    nextFrameBtn_->setIcon(loadIcon(QStringLiteral("next-frame")));
    nextFrameBtn_->setToolTip(tr("Next frame (Right arrow)"));

    fullscreenBtn_ = new TransportButton(this);
    fullscreenBtn_->setIcon(fullscreenIcon_);
    fullscreenBtn_->setToolTip(tr("Fullscreen (Ctrl+Return)"));

    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setMinimum(0);
    slider_->setMaximum(0);
    slider_->setValue(0);
    // Keyboard is reserved for transport (arrow-key stepping, J-K-L). If the
    // slider kept focus after a drag, arrows would move the slider instead of
    // stepping frames.
    slider_->setFocusPolicy(Qt::NoFocus);
    slider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal {"
        "  height: 4px; border-radius: 2px;"
        "  background: rgba(255,255,255,0.16);"
        "}"
        "QSlider::sub-page:horizontal {"
        "  height: 4px; border-radius: 2px;"
        "  background: rgba(255,255,255,0.72);"
        "}"
        "QSlider::handle:horizontal {"
        "  width: 12px; height: 12px; margin: -4px 0; border-radius: 6px;"
        "  background: #FFFFFF;"
        "}"
        "QSlider::handle:horizontal:hover { background: #FFFFFF; }"));

    frameLabel_ = new QLabel(this);
    frameLabel_->setFocusPolicy(Qt::NoFocus);
    // Monospaced so the frame counter doesn't jitter as digits change.
    frameLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    frameLabel_->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.78);"));
    frameLabel_->setMinimumWidth(96);
    frameLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(16, 8, 16, 8);
    row->setSpacing(8);
    row->addWidget(prevFrameBtn_);
    row->addWidget(playPauseBtn_);
    row->addWidget(nextFrameBtn_);
    row->addSpacing(8);
    row->addWidget(slider_, 1);
    row->addSpacing(4);
    row->addWidget(frameLabel_);
    row->addWidget(fullscreenBtn_);

    connect(prevFrameBtn_, &TransportButton::clicked, this, &TransportBar::prevFrameClicked);
    connect(playPauseBtn_, &TransportButton::clicked, this, &TransportBar::playPauseClicked);
    connect(nextFrameBtn_, &TransportButton::clicked, this, &TransportBar::nextFrameClicked);
    connect(fullscreenBtn_, &TransportButton::clicked, this, &TransportBar::fullscreenClicked);

    setControlsEnabled(false);
}

void TransportBar::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    playPauseBtn_->setIcon(playing ? pauseIcon_ : playIcon_);
    playPauseBtn_->setToolTip(playing ? tr("Pause (Space)") : tr("Play (Space)"));
}

void TransportBar::setFullscreen(bool fullscreen) {
    fullscreenBtn_->setIcon(fullscreen ? exitFullscreenIcon_ : fullscreenIcon_);
    fullscreenBtn_->setActive(fullscreen);
    fullscreenBtn_->setToolTip(fullscreen ? tr("Exit fullscreen (Ctrl+Return)")
                                          : tr("Fullscreen (Ctrl+Return)"));
}

void TransportBar::setFrameText(const QString& text) {
    frameLabel_->setText(text);
}

void TransportBar::setControlsEnabled(bool enabled) {
    prevFrameBtn_->setEnabledControl(enabled);
    playPauseBtn_->setEnabledControl(enabled);
    nextFrameBtn_->setEnabledControl(enabled);
    slider_->setEnabled(enabled);
}

void TransportBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Slightly lifted off the black stage so the bar reads as a surface,
    // with a hairline to separate it from the frame above.
    p.fillRect(rect(), QColor(18, 18, 18));
    p.setPen(QPen(QColor(255, 255, 255, 20), 1.0));
    p.drawLine(rect().topLeft(), rect().topRight());
}

} // namespace trace::ui
