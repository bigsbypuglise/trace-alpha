#include "app/Theme.h"

#include <QApplication>
#include <QByteArray>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QProxyStyle>
#include <QStringList>
#include <QStyleFactory>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace trace::app {
namespace {

// SH_UnderlineShortcut FROM THE SYSTEM SETTING (owner item 10, 2026-08-18).
// The menu mnemonics' underlines are Windows keyboard-access cues, and
// Windows' own default is to show them only after Alt is pressed -- controlled
// by SPI_GETKEYBOARDCUES. Fusion draws them unconditionally, which is what the
// owner saw. Answering the hint from the setting is better than hiding them
// outright: the menus look the way he wants by default AND the cues stay for
// keyboard and screen-reader users -- the population the phase 14 work exists
// to protect -- and for anyone whose Windows setting says "always show".
//
// The setting is queried live rather than cached, so a change in Windows
// Settings takes effect at the next menu paint with no restart.
class KeyboardCuesStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override {
#ifdef Q_OS_WIN
        if (hint == SH_UnderlineShortcut) {
            BOOL cues = TRUE;
            if (SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &cues, 0) && cues) return 1;
            // Cues off: underline only while Alt is held, which is the native
            // Windows behaviour the setting describes. QMenuBar repaints on
            // Alt, so the hint is re-asked at the right moments.
            return (QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier) ? 1 : 0;
        }
#endif
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

// The design package's own values, read out of
// assets/source/260817-trace-ui-v2/Trace-App-Mockups.html rather than chosen
// here. `#0A0B0D` is its `body` background, `#14161A` the strip's stated
// no-transparency fallback, `#1A1B20` the lifted surface, `#EDEFF1` the
// wordmark, `#C7CDD4` the menu and filename grey, `#5AC8E8` the accent, and
// `rgba(255,255,255,0.09)` the window's own hairline border.
const QColor kBackdrop(0x0A, 0x0B, 0x0D);
const QColor kSurface(0x14, 0x16, 0x1A);
const QColor kSurfaceRaised(0x1A, 0x1B, 0x20);
const QColor kTextPrimary(0xED, 0xEF, 0xF1);
const QColor kTextSecondary(0xC7, 0xCD, 0xD4);
const QColor kTextDisabled(0x6E, 0x74, 0x7B);
const QColor kAccent(0x5A, 0xC8, 0xE8);

QString g_resolvedFamily;

// GDI AND QT DISAGREE ABOUT WHETHER THIS FAMILY EXISTS, AND QT IS THE ONE THAT
// MATTERS. The first version of this file asserted that Windows has no family
// called "Segoe UI Variable" and mapped the design onto `Segoe UI Variable Text`
// instead. Half right, and the wrong half was the one that decided the code.
//
// GDI really does list only the three static optical cuts -- `Segoe UI Variable
// Display`, `... Text`, `... Small`, each with Light/Semilight/Semibold siblings
// -- and nothing under the plain name; that is what
// `InstalledFontCollection` reports on this box. But Qt 6 enumerates through
// DirectWrite and exposes the VARIABLE font under its typographic family name,
// so `QFontDatabase::families()` here contains exactly one match and it is
// `Segoe UI Variable`. The design's own CSS name resolves under Qt unchanged.
//
// Measured, not assumed: the first build asked for `Segoe UI Variable Text`,
// `hasFamily()` declined it, and the application silently ran on `Segoe UI`.
// The dev HUD's `font` field is what said so on its first run -- which is why
// that field exists, since a fallback to Segoe UI looks very nearly right.
//
// The design's name goes first and the static Text cut second, so a Qt build or
// platform plugin that enumerates the GDI way still lands on the right optical
// size rather than on plain Segoe UI. Which of the three that is, is settled by
// the mockup's own embedded TTF rather than by guidance: it carries axes `wght`
// 300..700 and `opsz` 5..36 with a DEFAULT OPTICAL SIZE OF 10.5, and 10.5 is
// squarely `Text` (Small is for captions, Display for headings). That agrees
// with the design's 12px UI type.
//
// THE REST OF THE CHAIN IS THE MOCKUP'S, in its order: the design names
// `"Segoe UI Variable", "Segoe UI", system-ui, "Helvetica Neue", Arial,
// sans-serif`. Qt resolves setFamilies() left to right the same way CSS does.
QFont buildApplicationFont(const QFont& base) {
    QFont f = base;
    // NO POINT SIZE IS SET, and that is deliberate rather than an omission.
    // TopChrome already records the reasoning for the menu bar and it applies to
    // the whole application: pinning the design's 12px would look right at 100%
    // and wrong at every other scale factor, and it would override the user's
    // Windows text-size setting -- which for a review tool someone sits in front
    // of all day is not ours to take away.
    const QStringList wanted{
        QStringLiteral("Segoe UI Variable"),
        QStringLiteral("Segoe UI Variable Text"),
        QStringLiteral("Segoe UI"),
        QStringLiteral("Helvetica Neue"),
        QStringLiteral("Arial"),
    };
    QStringList available;
    for (const QString& name : wanted) {
        if (QFontDatabase::hasFamily(name)) available << name;
    }
    if (available.isEmpty()) {
        g_resolvedFamily = base.family();
        return f;
    }
    f.setFamilies(available);
    g_resolvedFamily = available.front();
    return f;
}

} // namespace

QString Theme::resolvedFontFamily() { return g_resolvedFamily; }

void Theme::apply(QApplication& app) {
    // The application style, wrapped so menu mnemonic underlines honour
    // SPI_GETKEYBOARDCUES (owner item 10). Fusion stays the base -- this
    // replaces main.cpp's bare setStyle("Fusion") rather than adding a second
    // style site, keeping this file the one home for app-wide appearance.
    app.setStyle(new KeyboardCuesStyle(QStyleFactory::create(QStringLiteral("Fusion"))));

    app.setFont(buildApplicationFont(app.font()));

    QPalette p = app.palette();
    p.setColor(QPalette::Window, kSurface);
    p.setColor(QPalette::WindowText, kTextPrimary);
    p.setColor(QPalette::Base, kBackdrop);
    p.setColor(QPalette::AlternateBase, kSurfaceRaised);
    p.setColor(QPalette::Text, kTextPrimary);
    p.setColor(QPalette::Button, kSurfaceRaised);
    p.setColor(QPalette::ButtonText, kTextPrimary);
    p.setColor(QPalette::ToolTipBase, kSurfaceRaised);
    p.setColor(QPalette::ToolTipText, kTextPrimary);
    p.setColor(QPalette::PlaceholderText, kTextDisabled);
    p.setColor(QPalette::Disabled, QPalette::Text, kTextDisabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, kTextDisabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, kTextDisabled);

    // THE ACCENT, AND ONLY WHERE WINDOWS ITSELF PUTS ONE. The design uses
    // `#5AC8E8` on the played track and the thumb ring and NOWHERE ELSE, and
    // that restraint is recorded twice in the roadmap as a deliberate owner
    // decision. So this does not spray it over the interface: it becomes the
    // selection colour, which is the one thing a Windows 11 application is
    // expected to draw in its accent, and the link colour. Menu highlighting is
    // handled below and deliberately does NOT use it.
    //
    // HighlightedText must be dark: `#5AC8E8` has a high relative luminance, so
    // the near-black backdrop is what keeps selected text legible. Leaving it at
    // white would be unreadable on the one control this affects most, the
    // inspector's source-path field.
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, kBackdrop);
    p.setColor(QPalette::Link, kAccent);
    app.setPalette(p);

    // POPUP MENUS: the surface roadmap step 7 explicitly left to step 10.
    // TopChrome styles the menu BAR and says in its own comment that a bare
    // `QMenu` rule there would restyle every popup the bar owns, which is this
    // step's to do rather than that one's.
    //
    // The values are the design's own and not new ones: `#1A1B20` is the strip's
    // lighter stop, the 1px `rgba(255,255,255,0.09)` border is the window
    // border from screen-1, the 8px radius is the window's own, and the 3px/8px
    // item padding is the menu-bar item padding the strip already uses.
    //
    // SELECTION MATCHES THE MENU BAR RATHER THAN THE ACCENT. Step 7 shipped
    // `rgba(255,255,255,0.10)` for a highlighted menu-bar item; a popup that lit
    // up cyan while the bar above it lit up white would be two languages on one
    // gesture, and it would spend the accent on a transient hover -- which is
    // exactly the restraint the design asks for. Highlight stays the accent for
    // TEXT selection, where Windows does use it.
    app.setStyleSheet(QStringLiteral(
        "QMenu { background-color: #1A1B20; color: #C7CDD4;"
        " border: 1px solid rgba(255,255,255,0.09); border-radius: 8px;"
        " padding: 4px; }"
        "QMenu::item { background: transparent; padding: 5px 12px; margin: 1px 4px;"
        " border-radius: 4px; }"
        "QMenu::item:selected { background: rgba(255,255,255,0.10); color: #EDEFF1; }"
        "QMenu::item:disabled { color: #6E747B; }"
        "QMenu::separator { height: 1px; background: rgba(255,255,255,0.09);"
        " margin: 4px 10px; }"
        "QMenu::indicator { width: 14px; height: 14px; margin-left: 6px; }"));

    if (const QByteArray v = qgetenv("TRACE_THEME_LOG"); !v.isEmpty() && v != "0") {
        std::fprintf(stderr,
                     "trace-theme: font family '%s' (design asked for 'Segoe UI Variable'; "
                     "GDI lists only the Display/Text/Small cuts, Qt exposes the variable "
                     "font under the plain name)\n",
                     g_resolvedFamily.toLocal8Bit().constData());
        // WHAT QT SEES, NOT WHAT WINDOWS LISTS. The two disagree: GDI's
        // InstalledFontCollection reports `Segoe UI Variable Text` on this box
        // and Qt's own database is the only thing that decides whether
        // setFamilies() can use it. Printing the candidates is what turns "the
        // font silently fell back" from a guess into a reading.
        QStringList seen;
        for (const QString& fam : QFontDatabase::families()) {
            if (fam.contains(QStringLiteral("Segoe UI Variable"), Qt::CaseInsensitive)) seen << fam;
        }
        std::fprintf(stderr, "trace-theme: Qt sees %d 'Segoe UI Variable' famil%s%s%s\n",
                     static_cast<int>(seen.size()), seen.size() == 1 ? "y" : "ies",
                     seen.isEmpty() ? "" : ": ",
                     seen.join(QStringLiteral(" | ")).toLocal8Bit().constData());
    }
}

} // namespace trace::app
