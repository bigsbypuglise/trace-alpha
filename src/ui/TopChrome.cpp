#include "ui/TopChrome.h"

#include <QEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QLabel>
#include <QLinearGradient>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>

#include <algorithm>

namespace trace::ui {
namespace {

// EVERY NUMBER HERE IS THE DESIGN PACKAGE'S OWN.
// assets/source/260817-trace-ui-v2/Trace-App-Mockups.html, section screen-1:
// a 38px strip laid out `auto 1fr auto`, a 15px mark and the Trace wordmark at
// 12px/600 in #EDEFF1 with an 8px gap, 18px to the menus, the menu items at
// 12px in #C7CDD4 with 3px 8px padding, and the filename centred in #C7CDD4.
constexpr int kStripHeight = 38;
constexpr int kEdgePad = 12;
constexpr int kMarkPx = 15;
constexpr int kMarkToWordGap = 8;
constexpr int kWordToMenuGap = 18;

// The design's strip is `rgba(22,22,24,0.66)` fading to `rgba(22,22,24,0.04)`
// over a backdrop blur. A native window over the video swapchain cannot blend
// with it at all (plan section 18.4), so what is painted is the package's own
// stated fallback for the no-transparency case: solid #14161A. The vertical
// gradient is kept in the only form an opaque strip can carry it -- top
// slightly lighter than bottom -- so the shape of the design survives even
// though its transparency does not.
const QColor kStripTop(0x1A, 0x1B, 0x20);
const QColor kStripBottom(0x14, 0x16, 0x1A);
const QColor kHairline(255, 255, 255, 18);

const QColor kWordmark(0xED, 0xEF, 0xF1);
const QColor kTitle(0xC7, 0xCD, 0xD4);
// The empty-state mockup's own dimmer grey for "No media".
const QColor kTitleEmpty(0x6E, 0x74, 0x7B);

} // namespace

int TopChrome::stripHeightLogical() { return kStripHeight; }

TopChrome::TopChrome(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TopChrome"));
    // The measured configuration from plan section 18.4's table: a NATIVE
    // sibling over the child-HWND video surface is visible and hit-testable,
    // where a plain Qt child is neither. WA_DontCreateNativeAncestors keeps the
    // promotion to this widget alone -- without it Qt would make the ancestor
    // chain native too, which on TRACE_RENDERER=cpu would change the structure
    // of a signed-off paint path for no reason.
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    // Opaque, and saying so is what stops Qt asking the parent to paint behind
    // it -- under d3d11 the parent's client area is excluded by WS_CLIPCHILDREN
    // and there is nothing behind it to paint.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    // The strip must never take keyboard focus for itself. Spec phase 14's
    // accessibility proxies broke the Space bar exactly this way: Qt assigns
    // initial focus to the first focusable widget in the window, and every
    // transport widget is NoFocus by an old rule that is about anything
    // occupying the transport rather than about QPushButtons. The menu bar
    // inside still takes focus when the user asks for it with Alt, which is
    // what makes menus keyboard-reachable; it just does not take it at show
    // time.
    setFocusPolicy(Qt::NoFocus);

    markLabel_ = new QLabel(this);
    markLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    markLabel_->setFocusPolicy(Qt::NoFocus);
    // DELIBERATELY UNNAMED. It is decoration sitting immediately beside a
    // wordmark that already reads "Trace", and naming it would make a screen
    // reader announce the same word twice before reaching the menus.
    {
        QIcon icon;
        for (const int px : {kMarkPx, kMarkPx * 2}) {
            icon.addFile(QStringLiteral(":/ui/brand-mark-%1.png").arg(px), QSize(px, px));
        }
        markLabel_->setPixmap(icon.pixmap(QSize(kMarkPx, kMarkPx)));
    }

    wordmarkLabel_ = new QLabel(QStringLiteral("Trace"), this);
    wordmarkLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    wordmarkLabel_->setFocusPolicy(Qt::NoFocus);
    {
        QFont f = wordmarkLabel_->font();
        f.setWeight(QFont::DemiBold);
        wordmarkLabel_->setFont(f);
    }
    {
        QPalette pal = wordmarkLabel_->palette();
        pal.setColor(QPalette::WindowText, kWordmark);
        wordmarkLabel_->setPalette(pal);
    }

    titleLabel_ = new QLabel(this);
    titleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    titleLabel_->setFocusPolicy(Qt::NoFocus);
    titleLabel_->setAlignment(Qt::AlignCenter);

    menuBar_ = new QMenuBar(this);
    menuBar_->setNativeMenuBar(false);
    // Colours only. The FONT is left at the platform's, so the menu bar still
    // follows the user's Windows text-size setting and the per-monitor DPI --
    // pinning it to the design's 12px would look right at 100% and wrong
    // everywhere else. The selectors are QMenuBar-specific on purpose: a bare
    // `QMenu` rule here would restyle every popup the bar owns, and the popups
    // are real Windows menus that roadmap step 10 owns, not this step.
    menuBar_->setStyleSheet(QStringLiteral(
        "QMenuBar { background: transparent; color: #C7CDD4; border: 0px; }"
        "QMenuBar::item { background: transparent; color: #C7CDD4;"
        " padding: 3px 8px; margin: 0px; border-radius: 4px; }"
        "QMenuBar::item:selected { background: rgba(255,255,255,0.10); color: #EDEFF1; }"
        "QMenuBar::item:pressed { background: rgba(255,255,255,0.16); color: #EDEFF1; }"));

    if (parent) parent->installEventFilter(this);
    setMediaTitle(QString());
    hide();
}

void TopChrome::setMediaTitle(const QString& title) {
    mediaTitle_ = title;
    QPalette pal = titleLabel_->palette();
    pal.setColor(QPalette::WindowText, title.isEmpty() ? kTitleEmpty : kTitle);
    titleLabel_->setPalette(pal);
    // THE FULL NAME, NOT THE ELIDED ONE. relayout() elides the visible text to
    // fit, and a QLabel otherwise reports what it displays -- so a screen reader
    // would read the truncation rather than the file. This is the one place the
    // untruncated string is kept.
    titleLabel_->setAccessibleName(title.isEmpty() ? QObject::tr("No media") : title);
    relayout();
}

void TopChrome::setRevealed(bool revealed) {
    if (revealed == isVisible()) return;
    setVisible(revealed);
    // A native sibling starts life below the video surface in z-order; raising
    // it on each show is cheaper than trying to keep the order correct across
    // every path that can create or resize the surface.
    if (revealed) raise();
}

bool TopChrome::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parent() && event->type() == QEvent::Resize) relayout();
    return QWidget::eventFilter(watched, event);
}

void TopChrome::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void TopChrome::relayout() {
    auto* host = parentWidget();
    if (!host) return;
    if (geometry() != QRect(0, 0, host->width(), kStripHeight))
        setGeometry(0, 0, host->width(), kStripHeight);

    const auto centreY = [this](int h) { return (height() - h) / 2; };

    int x = kEdgePad;
    markLabel_->setGeometry(x, centreY(kMarkPx), kMarkPx, kMarkPx);
    x += kMarkPx + kMarkToWordGap;

    const int wordW = wordmarkLabel_->sizeHint().width();
    const int wordH = wordmarkLabel_->sizeHint().height();
    wordmarkLabel_->setGeometry(x, centreY(wordH), wordW, wordH);
    x += wordW + kWordToMenuGap;

    const int menuH = menuBar_->sizeHint().height();
    const int menuW = std::max(0, width() - x - kEdgePad);
    menuBar_->setGeometry(x, centreY(menuH), menuW, menuH);
    // Where the menu bar's own items stop. The filename may not run into them.
    const int leftGroupRight = x + menuBar_->sizeHint().width();

    // CENTRED ON THE STRIP, not on the space left over. The design's grid is
    // `auto 1fr auto` with window controls as the third column; Trace keeps the
    // native title bar until roadmap step 12, so there is no third column and
    // the honest reading of "justify-self: center" is the centre of the window.
    // Clamped so it can never overlap the menus, and elided rather than clipped
    // when a narrow window leaves it no room.
    const int avail = std::max(0, width() - leftGroupRight - kEdgePad * 2);
    const QString shown = mediaTitle_.isEmpty() ? QObject::tr("No media") : mediaTitle_;
    const QFontMetrics fm(titleLabel_->font());
    const QString elided = fm.elidedText(shown, Qt::ElideMiddle, avail);
    titleLabel_->setText(elided);
    const int titleW = std::min(avail, fm.horizontalAdvance(elided));
    const int titleH = fm.height();
    const int wanted = (width() - titleW) / 2;
    titleLabel_->setGeometry(std::max(leftGroupRight + kEdgePad, wanted),
                             centreY(titleH), titleW, titleH);
    titleLabel_->setVisible(avail > 0);
    // The menu bar spans the rest of the strip and is created after this label,
    // so without raising it the filename would be painted under a sibling whose
    // background only happens to be transparent.
    titleLabel_->raise();
}

void TopChrome::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0.0, kStripTop);
    g.setColorAt(1.0, kStripBottom);
    p.fillRect(rect(), g);
    // The bottom strip carries `border-top: 1px solid rgba(255,255,255,0.07)`;
    // this is its mirror, and it is what keeps the chrome legible where it meets
    // a bright frame.
    p.setPen(kHairline);
    p.drawLine(0, height() - 1, width(), height() - 1);
}

} // namespace trace::ui
