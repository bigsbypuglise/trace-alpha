#include "app/OverlayAccessibility.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QAction>
#include <QEvent>
#include <QKeySequence>
#include <QStringList>
#include <QWidget>

#include <cmath>

#include "render/OverlayModel.h"

namespace trace::app {
namespace {

using Region = trace::render::OverlayModel::Region;

// A zero-painting proxy. It exists to be FOUND -- by UI Automation, by the tab
// chain -- and to be activated. It draws nothing, because the compositor has
// already drawn the control it stands for.
//
// No paintEvent override and WA_NoSystemBackground: a plain QWidget with no
// background paints nothing at all, which is what is wanted. Overriding
// paintEvent to do nothing would work too and would invite someone to later
// draw a focus ring in it -- the focus ring belongs in the compositor, where
// the rest of the control's appearance lives.
// Qt::NoFocus, AND THE FIRST VERSION OF THIS FILE GOT IT WRONG IN A WAY THAT
// BROKE THE MOST-USED KEY IN THE APPLICATION.
//
// Plan section 19.7 asks for proxies in "a real tab chain", and the first cut
// gave them Qt::TabFocus and a Space/Enter activation handler. What actually
// happened: Qt assigns initial focus to the first widget in the tab chain that
// accepts it, the docked transport's widgets are all deliberately NoFocus, and
// the viewer is too -- so the REWIND PROXY took focus the moment the window
// opened, and the very first Space press read `speed -2.00x | Reverse Play`
// instead of playing. Not after a dialog, not after tabbing: the first press,
// on a fresh launch.
//
// CLAUDE.md has carried the rule this broke for months, and it names this
// failure exactly: "Transport widgets must not take keyboard focus ... keyboard
// belongs to frame stepping and J-K-L. If a new widget steals arrows/space,
// this is why." These ARE transport widgets. Reading it as a rule about
// QPushButtons rather than about anything occupying the transport was the
// mistake.
//
// So the proxies are announced and are not in the tab chain, and that is a
// smaller claim than section 19.7 made rather than a workaround for it. It is
// also the right trade for THIS application: a screen reader navigates the
// accessibility tree directly -- Narrator's scan mode reads elements whether or
// not they are tab stops -- so the announcement, the names and the states all
// survive. What is given up is Tab-to-activate, and every command the proxies
// stand for already has a keyboard shortcut that works from anywhere, which
// ShortcutTable::rows() enumerates and the Keyboard Shortcuts window prints.
// A screen-reader user drives Trace by shortcut; these say what the thing on
// screen IS.
//
// If Tab reachability is ever wanted, it cannot simply be switched back on:
// something has to hold focus by default first, and that is a decision about
// the viewer, not about this file.
class ProxyWidget final : public QWidget {
public:
    ProxyWidget(QWidget* parent, QAction* action, QAccessible::Role role)
        : QWidget(parent), action_(action), role_(role) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
    }

    QAction* action() const { return action_; }
    QAccessible::Role role() const { return role_; }

private:
    QAction* action_ = nullptr;
    QAccessible::Role role_ = QAccessible::Button;
};

// WITHOUT THIS EVERY CONTROL ANNOUNCES AS "group", WHICH IS TRUE AND USELESS.
//
// A plain QWidget maps to QAccessible::Client, which UI Automation reports as
// ControlType.Group -- measured, before this existed, with uiatree.ps1: all five
// proxies read `Group "Rewind"`, `Group "Play"` and so on. A screen reader then
// says "Rewind, group", which tells the listener the name of something and not
// what it is or what it does.
//
// The role has to come from an accessible interface rather than from a property
// on the widget, because Qt has no setAccessibleRole(). This is the smallest
// thing that supplies one: everything except role() and state() is
// QAccessibleWidget's, including the name and description, so those still come
// from setAccessibleName/setAccessibleDescription and there is no second copy of
// either string.
// QAccessibleWidget ALREADY inherits QAccessibleActionInterface, so this
// overrides its action virtuals rather than inheriting the interface a second
// time -- which does not compile, and would have been two answers to one
// question if it had.
class ProxyAccessible final : public QAccessibleWidget {
public:
    explicit ProxyAccessible(ProxyWidget* widget)
        : QAccessibleWidget(widget, widget->role()) {}

    // A BUTTON THAT ADVERTISES Invoke AND DOES NOTHING IS THE showInfo FAILURE
    // PHASE 2 DELETED, in a place nobody sighted would ever notice.
    //
    // Giving the proxies a Button role makes Qt's UIA bridge advertise the
    // Invoke pattern -- measured: the Rewind proxy reported
    // `InvokePatternIdentifiers.Pattern` before this existed. But
    // QAccessibleWidget's own action handling knows how to press a
    // QAbstractButton, and these are plain QWidgets, so invoking one did
    // nothing at all. A screen reader user would have activated Rewind and got
    // silence.
    //
    // This is also the activation route the tab chain was giving up: assistive
    // technology invokes an element directly, WITHOUT focus, so it costs
    // nothing of what the NoFocus decision above was protecting.
    QStringList actionNames() const override {
        if (auto* proxy = dynamic_cast<ProxyWidget*>(object())) {
            if (proxy->action() && proxy->action()->isEnabled()) {
                return {QAccessibleActionInterface::pressAction()};
            }
        }
        return {};
    }

    void doAction(const QString& name) override {
        if (name != QAccessibleActionInterface::pressAction()) return;
        if (auto* proxy = dynamic_cast<ProxyWidget*>(object())) {
            if (proxy->action() && proxy->action()->isEnabled()) proxy->action()->trigger();
        }
    }

    QString localizedActionName(const QString& name) const override {
        if (name == QAccessibleActionInterface::pressAction()) {
            return OverlayAccessibility::tr("Press");
        }
        return QAccessibleActionInterface::localizedActionName(name);
    }

    QStringList keyBindingsForAction(const QString& name) const override {
        if (name != QAccessibleActionInterface::pressAction()) return {};
        if (auto* proxy = dynamic_cast<ProxyWidget*>(object())) {
            // FROM THE QAction, so the announced key and the working key are
            // the same string by construction. The transport actions carry no
            // shortcut of their own -- J, K, L and Space are dispatched by
            // ShortcutTable -- so this is usually empty and the DESCRIPTION
            // names the key instead. Reported anyway, because an action that
            // gains a shortcut later should not need this remembered.
            if (proxy->action()) {
                QStringList keys;
                for (const auto& sequence : proxy->action()->shortcuts()) {
                    keys << sequence.toString(QKeySequence::NativeText);
                }
                return keys;
            }
        }
        return {};
    }

    // Qt's Windows UIA bridge fills HelpText from QAccessible::Help and
    // FullDescription from QAccessible::Description. setAccessibleDescription()
    // therefore reaches FullDescription only -- which Narrator does read, but
    // which the older UIA client APIs cannot, so it read as EMPTY on every
    // check. Answering Help with the same string puts it in both places from
    // one source, and makes the thing a screen reader will say verifiable
    // without a person listening.
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Help) {
            if (auto* widget = qobject_cast<QWidget*>(object())) {
                const QString description = widget->accessibleDescription();
                if (!description.isEmpty()) return description;
            }
        }
        return QAccessibleWidget::text(type);
    }

    QAccessible::State state() const override {
        QAccessible::State s = QAccessibleWidget::state();
        // Read from the QAction, so a control the application has disabled
        // announces as disabled rather than as available-but-inert. Nothing is
        // mirrored here: the action is the one source, exactly as it is for the
        // name and for the menus.
        if (auto* proxy = dynamic_cast<ProxyWidget*>(object())) {
            if (proxy->action()) {
                s.disabled = !proxy->action()->isEnabled();
                if (proxy->action()->isCheckable()) {
                    s.checkable = true;
                    s.checked = proxy->action()->isChecked();
                }
            }
        }
        // Never focusable, and it must SAY so: the proxies are deliberately out
        // of the tab chain (see the note above), and a screen reader that
        // believed otherwise would offer a navigation that does nothing.
        s.focusable = false;
        s.focused = false;
        return s;
    }
};

// dynamic_cast, NOT the className argument. ProxyWidget has no Q_OBJECT macro --
// it is a local type in an anonymous namespace and moc does not reach it -- so
// its metaObject()->className() is "QWidget", which would match every plain
// widget in the application. Qt asks each installed factory in turn and takes
// the first non-null answer, so returning null for everything else leaves every
// other widget with whatever Qt already gave it.
QAccessibleInterface* proxyAccessibleFactory(const QString& className, QObject* object) {
    Q_UNUSED(className);
    if (auto* proxy = dynamic_cast<ProxyWidget*>(object)) return new ProxyAccessible(proxy);
    return nullptr;
}

} // namespace

OverlayAccessibility::OverlayAccessibility(QWidget* host,
                                           const trace::render::OverlayModel* model,
                                           Commands commands)
    : QObject(host), host_(host), model_(model), commands_(std::move(commands)) {
    if (!host_ || !model_) return;

    // IN THE ORDER controlRects() RETURNS THEM, which is strictly LEFT TO
    // RIGHT along the strip: Rewind, Play/Pause, Fast-forward, Mute, Loop,
    // Timeline, Fullscreen, Share. A screen reader walks children in creation
    // order, so this IS the reading order -- and a reading order that does not
    // match the visual one is the classic way an accessible interface becomes
    // unusable while passing every check.
    // Installed once for the process. Qt keeps a list of factories and asks
    // each in turn, so this is additive -- every other widget in the
    // application keeps whatever Qt already gave it.
    static bool factoryInstalled = false;
    if (!factoryInstalled) {
        QAccessible::installFactory(&proxyAccessibleFactory);
        factoryInstalled = true;
    }

    struct Spec {
        Region region;
        QAction* action;
        const char* fallbackName;
        const char* description;
        QAccessible::Role role;
    };
    // Go to Start and Go to End are no longer on the strip (owner item 15,
    // 2026-08-18), so they are no longer announced as strip controls -- Home
    // and End remain the keyboard route and their QActions still exist.
    const Spec specs[] = {
        {Region::Rewind, commands_.rewind, QT_TR_NOOP("Rewind"),
         QT_TR_NOOP("Rewind. Each press goes faster: 2x, 5x, 10x, 30x. The J key "
                    "does the same from anywhere, starting at 1x."),
         QAccessible::Button},
        {Region::PlayPause, commands_.playPause, QT_TR_NOOP("Play"),
         QT_TR_NOOP("Play or pause. The Space bar does the same from anywhere."),
         QAccessible::Button},
        {Region::FastForward, commands_.fastForward, QT_TR_NOOP("Fast-forward"),
         QT_TR_NOOP("Fast-forward. Each press goes faster: 2x, 5x, 10x, 30x. The L "
                    "key does the same from anywhere, starting at 1x."),
         QAccessible::Button},
        // CheckBox, not Button, and the role is the honest one: this control
        // reports a STATE as well as running a command, and a screen reader
        // that says "Mute, checked" is telling the user something a Button role
        // would hide. QAccessibleWidget maps the action's checked state through
        // ProxyAccessible::state(), so the announcement follows the audio
        // rather than the last click.
        {Region::Mute, commands_.mute, QT_TR_NOOP("Mute"),
         QT_TR_NOOP("Mute or unmute the audio. The M key does the same from anywhere."),
         QAccessible::CheckBox},
        // CheckBox for the same reason Mute is: it reports a state, and Loop's
        // is the one the strip shows by COLOUR rather than by a second glyph --
        // which a screen reader cannot see at all, so the role carrying it is
        // doing more work here than anywhere else on the strip.
        {Region::Loop, commands_.loop, QT_TR_NOOP("Loop"),
         QT_TR_NOOP("Repeat playback when it reaches the end. The setting is "
                    "remembered between files and between sessions."),
         QAccessible::CheckBox},
        // Slider, not Button: it is a position along a range, and that is what
        // it should announce as even though this proxy carries no value. A
        // Button role here would be a lie a screen reader repeats.
        {Region::Timeline, nullptr, QT_TR_NOOP("Timeline"),
         QT_TR_NOOP("Playback position. Use Left and Right to step one frame, "
                    "or Ctrl+G to go to a frame."),
         QAccessible::Slider},
        {Region::Fullscreen, commands_.fullscreen, QT_TR_NOOP("Toggle Fullscreen"),
         QT_TR_NOOP("Enter or leave fullscreen. F11 does the same from anywhere, "
                    "and Escape leaves it."),
         QAccessible::CheckBox},
        // SHARE IS ANNOUNCED AND IS NOT ACTIVATED FROM HERE, and the
        // description says so rather than leaving a control that answers to
        // nothing. Its command is a QMenu, and a menu needs a screen position
        // to pop at -- which the keyboard has no answer for. The menu bar's
        // File > Share is the keyboard route, it is a real QMenu, and it has
        // been the accessible Share surface since phase 8 said so.
        {Region::Share, nullptr, QT_TR_NOOP("Share"),
         QT_TR_NOOP("Share options. Use the File menu, Share, to reach them from "
                    "the keyboard."),
         QAccessible::ButtonMenu},
    };

    for (const auto& spec : specs) {
        auto* proxy = new ProxyWidget(host_, spec.action, spec.role);
        // The NAME comes from the action when there is one, so it is the same
        // string the menus and tooltips use and cannot drift into a second
        // wording. Mnemonics stripped: '&' is markup for a menu, not part of
        // what a control is called.
        QString name = spec.action ? spec.action->text() : tr(spec.fallbackName);
        name.remove(QLatin1Char('&'));
        proxy->setAccessibleName(name);
        proxy->setAccessibleDescription(tr(spec.description));
        proxy->show();
        proxies_.push_back(proxy);

        // AND IT KEEPS FOLLOWING THE ACTION. Play/Pause is the case that makes
        // this necessary -- MainWindow rewrites playPauseAction_'s text on every
        // state change, so a name copied once would say "Play" over a running
        // file for the rest of the session. QAction::changed fires for text,
        // enabled and checked alike, so the same connection covers a control
        // that is later disabled or made checkable.
        if (spec.action) {
            QObject::connect(spec.action, &QAction::changed, proxy, [proxy, spec]() {
                QString current = spec.action->text();
                current.remove(QLatin1Char('&'));
                if (!current.isEmpty()) proxy->setAccessibleName(current);
            });
        }
    }

    // SYNCED ON PAINT, GATED ON THE LAYOUT REVISION -- and syncing on Resize
    // alone was measured to be wrong, in a way that left the tree looking
    // complete.
    //
    // OverlayModel::layout() runs inside buildFrame(), i.e. DURING the paint. A
    // proxy repositioned from the viewer's resizeEvent therefore reads the rects
    // from before the layout that resize is about to cause -- and at startup it
    // reads them before there has been any layout at all, so controlRects()
    // returns nothing and sync() parks every proxy. uiatree.ps1 on that build
    // found all five controls, correctly named and in the right reading order,
    // each with an EMPTY bounding rectangle: a screen reader could say what
    // every control was called and not where any of them were.
    //
    // Phase 10's trap, in a third costume. `display` is measured by the paint;
    // `lastDrawSize` is measured by the paint; the overlay's rects are computed
    // by the paint. Anything that reads them has to run after one.
    //
    // The cost is an integer compare per paint, which is why the revision
    // exists rather than a geometry comparison -- the same promise
    // atlasRevision() makes about the pixels.
    host_->installEventFilter(this);
    sync();
}

bool OverlayAccessibility::eventFilter(QObject* watched, QEvent* event) {
    if (watched == host_) {
        switch (event->type()) {
            case QEvent::Paint:
                // AFTER the paint, not before: buildFrame() runs inside it.
                // Queued rather than called here so this filter never sits in
                // front of a present.
                if (model_ && model_->layoutRevision() != lastLayoutRevision_) {
                    QMetaObject::invokeMethod(this, [this]() { sync(); },
                                              Qt::QueuedConnection);
                }
                break;
            case QEvent::Show:
            case QEvent::Resize:
                sync();
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void OverlayAccessibility::sync() {
    if (!host_ || !model_ || proxies_.empty()) return;
    lastLayoutRevision_ = model_->layoutRevision();

    const auto rects = model_->controlRects();
    if (rects.size() != proxies_.size()) {
        // The overlay has not laid out yet (no surface size), or the model
        // gained a control this was not rebuilt for. Park the proxies rather
        // than positioning some of them: a partially-correct accessible tree is
        // worse than an absent one, because it reads as complete.
        for (auto* proxy : proxies_) proxy->setGeometry(QRect());
        return;
    }

    // Device pixels to the host's LOGICAL coordinates. The model lays out in
    // surface device pixels -- what the swapchain is sized to -- and QWidget
    // geometry is logical, so this is the one conversion, done here rather than
    // by asking the model for a second set of rects in different units.
    const double dpr = model_->devicePixelRatio() > 0.0 ? model_->devicePixelRatio() : 1.0;
    for (size_t i = 0; i < rects.size(); ++i) {
        const QRectF& dst = rects[i].dst;
        const QRect logical(static_cast<int>(std::floor(dst.left() / dpr)),
                            static_cast<int>(std::floor(dst.top() / dpr)),
                            std::max(1, static_cast<int>(std::ceil(dst.width() / dpr))),
                            std::max(1, static_cast<int>(std::ceil(dst.height() / dpr))));
        proxies_[i]->setGeometry(logical);
    }
}

} // namespace trace::app
