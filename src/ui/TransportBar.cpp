#include "ui/TransportBar.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QFile>
#include <QFontDatabase>
#include <QProxyStyle>
#include <QTimer>

namespace trace::ui {
namespace {

// Design tokens. STATE VALUES are the approved set's
// (assets/source/original-design-package/base-ui-icons/README.txt);
// CONTROL GEOMETRY is still the first-pass set's, and that is deliberate.
//
// The approved package specifies 34x34 utility targets and a 44x44 play/pause
// inside a rounded panel -- but that is the geometry of the FLOATING transport,
// which is what spec phase 6 puts on screen when it removes this bar from the
// layout entirely. Re-laying-out a bar that is about to be deleted is churn the
// spec does not ask for at phase 2, and it would change the video rect (and
// therefore `win WxH`, and therefore every scrub baseline) for a widget with no
// future. The state opacities do apply to the artwork itself and are adopted.
constexpr int kHit = 44;
constexpr int kHitRadius = 10;
constexpr int kCenter = 60;
constexpr int kCenterRadius = 15;
constexpr int kMark = 24;        // icon mark inside a standard control
constexpr int kCenterMark = 26;  // icon mark inside the center control

constexpr double kOpacityRest = 0.82;
constexpr double kOpacityFull = 1.00;
constexpr double kOpacityDisabled = 0.28;

constexpr double kBgHover = 0.09;
constexpr double kBgPressed = 0.17;
constexpr double kBgActive = 0.12;
constexpr double kStrokeActive = 0.22;

constexpr int kBarHeight = 76;

// How long the shuttle-rate indicator stays up. The spec calls it a "temporary
// indicator" and does not name a duration; this is long enough to read at a
// glance and short enough that walking the ladder shows each rung rather than
// leaving the last one on screen for the rest of the run.
constexpr int kRateFlashMs = 1200;

// Qt's default slider bindings are wrong for a video timeline: left-clicking
// the groove page-steps (SH_Slider_PageSetButtons), so a click far down the
// track nudged the playhead by pageStep frames instead of going there, and
// only middle-click jumped to the clicked position. Swapping the two hints
// hands the work to QSlider's own machinery, which maps the click through
// QStyle::sliderValueFromPosition and then continues as a drag from that
// point -- so groove/handle geometry, the stylesheet's handle width and RTL
// all stay correct without hand-rolled math.
class AbsoluteSeekSliderStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint,
                  const QStyleOption* option,
                  const QWidget* widget,
                  QStyleHintReturn* returnData) const override {
        if (hint == SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        if (hint == SH_Slider_PageSetButtons) return Qt::NoButton;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

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

// The approved base UI set ships 24px and 48px only. The superseded first-pass
// set did ship 72px, and until spec phase 5 two frame-step glyphs from it were
// still embedded; nothing in the tree has a -72 now, so this branch is dead
// weight in the resources it can see and live insurance against the next
// package that does. Adding a resource that is not there is not an error in Qt,
// it is silently nothing -- which would make "is the 3x file being used"
// unanswerable by reading. Asking is one line and makes the answer visible.
QIcon TransportBar::loadIcon(const QString& baseName) {
    QIcon icon;
    icon.addFile(QStringLiteral(":/ui/%1-24.png").arg(baseName), QSize(24, 24));
    icon.addFile(QStringLiteral(":/ui/%1-48.png").arg(baseName), QSize(48, 48));
    const QString at3x = QStringLiteral(":/ui/%1-72.png").arg(baseName);
    if (QFile::exists(at3x)) icon.addFile(at3x, QSize(72, 72));
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

    // Spec phase 5, the mirror of phase 4 below. This control used to step one
    // frame backward; it is Rewind now, and the artwork moved in the same change
    // because artwork follows behaviour. Frame stepping is the Left arrow -- the
    // command is untouched, only the button that used to reach it is gone.
    rewindBtn_ = new TransportButton(this);
    rewindBtn_->setIcon(loadIcon(QStringLiteral("rewind")));
    rewindBtn_->setToolTip(tr("Rewind — 2x, 5x, 10x, 30x (J)"));

    playPauseBtn_ = new TransportButton(this);
    playPauseBtn_->setCenterControl(true);
    playPauseBtn_->setIcon(playIcon_);
    playPauseBtn_->setToolTip(tr("Play / Pause (Space)"));

    // Spec phase 4. This control used to step one frame forward; it is
    // Fast-forward now, and the artwork moved in the same change because
    // artwork follows behaviour. Frame stepping is the Right arrow -- the
    // command is untouched, only the button that used to reach it is gone.
    fastForwardBtn_ = new TransportButton(this);
    fastForwardBtn_->setIcon(loadIcon(QStringLiteral("fast-forward")));
    fastForwardBtn_->setToolTip(tr("Fast-forward — 2x, 5x, 10x, 30x (L)"));

    fullscreenBtn_ = new TransportButton(this);
    fullscreenBtn_->setIcon(fullscreenIcon_);
    fullscreenBtn_->setToolTip(tr("Fullscreen (F11)"));

    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setMinimum(0);
    slider_->setMaximum(0);
    slider_->setValue(0);
    // Click anywhere on the track to seek there, then drag from that point.
    // Applied before the stylesheet so QStyleSheetStyle wraps this proxy
    // rather than replacing it. Parented to the slider so it is not leaked.
    auto* seekStyle = new AbsoluteSeekSliderStyle;
    seekStyle->setParent(slider_);
    slider_->setStyle(seekStyle);
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

    // The spec's temporary rate indicator. A FIXED width, held whether or not
    // there is text in it: a label that collapses when it clears would move the
    // slider under the pointer every time a shuttle run ended, which is the
    // same class of fault as the handle being written back mid-drag.
    rateLabel_ = new QLabel(this);
    rateLabel_->setFocusPolicy(Qt::NoFocus);
    rateLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    rateLabel_->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.92);"));
    rateLabel_->setFixedWidth(48);
    rateLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    rateTimer_ = new QTimer(this);
    rateTimer_->setSingleShot(true);
    rateTimer_->setInterval(kRateFlashMs);
    connect(rateTimer_, &QTimer::timeout, this, [this]() { rateLabel_->clear(); });

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(16, 8, 16, 8);
    row->setSpacing(8);
    row->addWidget(rewindBtn_);
    row->addWidget(playPauseBtn_);
    row->addWidget(fastForwardBtn_);
    row->addSpacing(8);
    row->addWidget(slider_, 1);
    row->addSpacing(4);
    row->addWidget(rateLabel_);
    row->addWidget(frameLabel_);
    row->addWidget(fullscreenBtn_);

    connect(rewindBtn_, &TransportButton::clicked, this, &TransportBar::rewindClicked);
    connect(playPauseBtn_, &TransportButton::clicked, this, &TransportBar::playPauseClicked);
    connect(fastForwardBtn_, &TransportButton::clicked, this, &TransportBar::fastForwardClicked);
    connect(fullscreenBtn_, &TransportButton::clicked, this, &TransportBar::fullscreenClicked);

    setControlsEnabled(false);
}

// Shown on a shuttle press and cleared on a timer. Deliberately NOT driven from
// syncTransportBar: that runs several times a second, and an indicator whose
// visibility is recomputed on every HUD refresh would either flicker or need a
// second piece of state saying when it was last set. One call at the press,
// one timer.
int TransportBar::rateFlashMs() { return kRateFlashMs; }

void TransportBar::flashRate(const QString& text) {
    if (text.isEmpty()) {
        rateTimer_->stop();
        rateLabel_->clear();
        return;
    }
    rateLabel_->setText(text);
    rateTimer_->start();
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
    fullscreenBtn_->setToolTip(fullscreen ? tr("Exit fullscreen (F11)")
                                          : tr("Fullscreen (F11)"));
}

void TransportBar::setFrameText(const QString& text) {
    frameLabel_->setText(text);
}

void TransportBar::setControlsEnabled(bool enabled) {
    rewindBtn_->setEnabledControl(enabled);
    playPauseBtn_->setEnabledControl(enabled);
    fastForwardBtn_->setEnabledControl(enabled);
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
