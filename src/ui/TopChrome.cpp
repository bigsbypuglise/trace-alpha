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
#include <QSettings>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace trace::ui {
namespace {

// EVERY NUMBER HERE IS THE DESIGN PACKAGE'S OWN.
// assets/source/260817-trace-ui-v2/Trace-App-Mockups.html, section screen-1:
// a 38px strip laid out `auto 1fr auto`, the menu items at 12px in #C7CDD4
// with 3px 8px padding, and the filename centred in #C7CDD4.
//
// THE MARK AND WORDMARK ARE GONE (owner item 1's approved interim,
// 2026-08-18): while the native title bar remains -- its removal is roadmap
// step 12, the frameless-window milestone -- the app identity appears twice,
// and the strip's copy is the redundant one. The menus now start at the edge
// pad. The design's 15px mark and 12px/600 wordmark return with step 12, when
// the strip becomes the only header.
constexpr int kStripHeight = 38;
constexpr int kEdgePad = 12;

// THE STRIP'S OWN CONTENT IS THE DESIGN PACKAGE'S SOLID FALLBACK, AND THE
// TRANSLUCENCY IS THE WINDOW'S, NOT THE PAINT'S (owner item 8, option B,
// 2026-08-19). The strip paints this dark vertical gradient opaquely; the DWM
// layered-window alpha then blends the whole strip -- content, labels,
// filename -- with the real video beneath it on the d3d11 default. That
// composes into a uniform dark scrim over the picture, which the 2026-08-19
// alpha sweep measured as MORE legible over bright busy footage than the
// painted backdrop blur it replaces: the blur was itself a bright copy of the
// video, so a resting alpha under it counted the video twice and washed the
// menu labels out by a215, while the solid content stays readable there.
// That measurement is why route 2's StripBackdrop was REMOVED rather than
// composed with the resting alpha -- record in
// docs/ui-feedback-260818-progress.md, item 8.
const QColor kStripTop(0x1A, 0x1B, 0x20);
const QColor kStripBottom(0x14, 0x16, 0x1A);
const QColor kHairline(255, 255, 255, 18);

// THE RESTING ALPHA, PICKED FROM LEGIBILITY AND NOT FROM THE DESIGN'S CSS
// (owner instruction): the design's rgba(22,22,24,0.66) scrim and a uniform
// window alpha are different mechanisms, so its 0.66 does not transfer. Chosen
// from a 155..255 sweep over the two hardest bands in the asset set -- the 4K
// milk splash and the Marinelaverse end tag's bright saturated detail -- as
// the lowest value at which every menu label stays cleanly separable where a
// near-white element crosses it. 200 was marginal on the worst spot; 230
// barely reads as translucent. 215 (~84%) is the pick.
constexpr int kRestingAlpha = 215;

const QColor kTitle(0xC7, 0xCD, 0xD4);
// The empty-state mockup's own dimmer grey for "No media".
const QColor kTitleEmpty(0x6E, 0x74, 0x7B);

// Owner item 11 experiment. Default ON so the experiment measures what would
// ship; =0 is the control and never touches the window's ex-style.
bool fadeKnob() {
    static const bool v = qgetenv("TRACE_TOPCHROME_FADE") != "0";
    return v;
}

// Pin the layered alpha for measurement: a mid-fade frame is an 82ms window,
// a pinned alpha is a stable state the harness can capture at leisure. -1
// means not pinned. =255 doubles as the override forcing the opaque resting
// strip.
int pinnedAlpha() {
    static const int v = [] {
        const QByteArray e = qgetenv("TRACE_TOPCHROME_ALPHA");
        if (e.isEmpty()) return -1;
        bool ok = false;
        const int n = e.toInt(&ok);
        return ok ? std::clamp(n, 0, 255) : -1;
    }();
    return v;
}

// Windows' transparency setting, tri-state: -1 no value found, 0 off, 1 on.
// Moved here from ViewerWidget when the strip backdrop was removed -- the
// setting used to gate the painted blur and now gates the resting alpha, so
// its reader lives with its one consumer.
//
// ABSENT MEANS ON. `EnableTransparency` is written when the user touches the
// toggle, so a machine that never has carries no value -- and the Windows
// default is transparency on. Absent is nevertheless kept distinct, because a
// path with a typo in it and an untouched machine produce the same boolean,
// and the dev HUD label is the only proof the right key was read at all.
//
// QSettings::NativeFormat here is registry READING, not a settings home --
// spec phase 11's rule is about writing. Nothing here writes.
int readWindowsTransparency() {
#ifdef Q_OS_WIN
    QSettings personalize(
        QStringLiteral(
            R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)"),
        QSettings::NativeFormat);
    const QVariant v = personalize.value(QStringLiteral("EnableTransparency"));
    if (!v.isValid()) return -1;
    return v.toInt() != 0 ? 1 : 0;
#else
    return -1;
#endif
}

// Cached because the answer is asked on every fade tick, and invalidated by
// onSystemAppearanceChanged() (WM_SETTINGCHANGE) -- so the answer is live
// without the registry read being.
int& windowsTransparencyCache() {
    static int v = readWindowsTransparency();
    return v;
}

// The alpha a fully revealed strip settles at. Only an explicit registry 0
// forces opaque; absent is on, per above.
int restingCeiling() {
    return windowsTransparencyCache() != 0 ? kRestingAlpha : 255;
}

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
    // Apply the current alpha BEFORE the window maps. The reveal edge fires
    // at the START of the model's fade-in, when its opacity is still 0, so
    // this is what makes the strip arrive transparent and ramp rather than
    // popping opaque for the first tick. On the very first show fadeAlpha_ is
    // its initial 0 for the same reason.
    if (revealed && fadeEnabled()) {
        fadeAlpha_ = effectiveAlpha();
        applyLayeredAlpha(fadeAlpha_);
    }
    setVisible(revealed);
    // A native sibling starts life below the video surface in z-order; raising
    // it on each show is cheaper than trying to keep the order correct across
    // every path that can create or resize the surface.
    if (revealed) raise();
}

bool TopChrome::fadeEnabled() { return fadeKnob(); }

void TopChrome::applyLayeredAlpha(int alpha) {
#ifdef Q_OS_WIN
    // winId() is safe here: the widget is WA_NativeWindow from the
    // constructor, so this forces nothing that was not already forced.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    // Uniform alpha, LWA_ALPHA only. The window keeps painting normally into
    // its own surface; the compositor applies the alpha. UpdateLayeredWindow
    // -- the per-pixel variant -- would take over the paint path entirely and
    // is exactly what this experiment is shaped to avoid.
    SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
#else
    Q_UNUSED(alpha);
#endif
}

// One expression for "what should the window's alpha read right now", shared
// by the fade tick, the reveal edge and the settings-change reapply. The
// ceiling is what makes the resting strip translucent: opacity 1.0 maps to
// restingCeiling(), so the fade is a 0 -> resting ramp rather than a
// 0 -> opaque one with a step at the end.
//
// WITH NO MEDIA THE STRIP RESTS OPAQUE, and mediaTitle_ is the media-presence
// answer the strip already holds (empty means no media, the same encoding
// setMediaTitle draws the dimmed "No media" from). Two reasons, one of them an
// instrument: there is no picture behind the empty stage for translucency to
// show, and the empty state is the one surface where the whole window is
// byte-comparable across backends -- the step 11 record's 0-differing-pixels
// standard. A translucent strip over the black stage would break that
// comparison for a visual difference nobody can see; opaque keeps the
// instrument. The owner-accepted d3d11/cpu divergence is over VIDEO.
int TopChrome::effectiveAlpha() const {
    if (pinnedAlpha() >= 0) return pinnedAlpha();
    const int ceiling = mediaTitle_.isEmpty() ? 255 : restingCeiling();
    return static_cast<int>(
        std::lround(std::clamp(fadeOpacity01_, 0.0, 1.0) * ceiling));
}

void TopChrome::setFadeOpacity(double opacity01) {
    if (!fadeEnabled()) return;
    fadeOpacity01_ = opacity01;
    const int alpha = effectiveAlpha();
    if (alpha == fadeAlpha_) return;
    fadeAlpha_ = alpha;
    applyLayeredAlpha(alpha);
}

void TopChrome::onSystemAppearanceChanged() {
    if (!fadeEnabled()) return;
    const int was = windowsTransparencyCache();
    const int now = readWindowsTransparency();
    if (now == was) return;
    windowsTransparencyCache() = now;
    // Reapply only if the EFFECTIVE ceiling moved -- absent and on are both on,
    // so a value appearing where there was none changes the reading without
    // changing the picture. A settled strip has no next animation tick, so the
    // reapply cannot wait for one; fadeOpacity01_ is held for exactly this.
    const int alpha = effectiveAlpha();
    if (alpha == fadeAlpha_) return;
    fadeAlpha_ = alpha;
    applyLayeredAlpha(alpha);
}

QString TopChrome::stripStateLabel() const {
    if (!fadeEnabled()) return QStringLiteral("opaque (fade off)");
    if (pinnedAlpha() >= 0) return QStringLiteral("a%1 (env)").arg(pinnedAlpha());
    switch (windowsTransparencyCache()) {
        case 0:  return QStringLiteral("opaque (windows)");
        case 1:  return QStringLiteral("a%1").arg(kRestingAlpha);
        // No value under the Personalize key: on, per the Windows default --
        // printed distinctly, because this is also what a wrong key path would
        // read and the two must not look alike.
        default: return QStringLiteral("a%1 (unset)").arg(kRestingAlpha);
    }
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

    // The menus lead the strip now -- the mark and wordmark left with owner
    // item 1's interim (the title bar already says Trace).
    int x = kEdgePad;
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
