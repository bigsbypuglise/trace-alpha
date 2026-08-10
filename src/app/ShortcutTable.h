#pragma once

#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <Qt>

#include <functional>
#include <vector>

class QKeyEvent;

namespace trace::app {

// The headings a Keyboard Shortcuts window prints (spec phase 13).
enum class ShortcutGroup { File, Playback, Stepping, Shuttle, View };

// One row of Trace's keyboard contract.
//
// A row is DOCUMENTATION first and DISPATCH second, and separating the two is
// the whole point. Every shortcut in the app appears here exactly once, but only
// the rows keyPressEvent still owns carry a handler; the rest are carried by a
// QAction, which Qt has already dispatched by the time keyPressEvent is reached.
//
// An action-owned row points AT its action rather than copying the action's keys
// and text, so it cannot drift. A second hand-written list is precisely what
// phase 13 must not have to maintain, and it is what a Keyboard Shortcuts window
// built by reading keyPressEvent's switch would have forced.
struct ShortcutRow {
    ShortcutGroup group = ShortcutGroup::View;

    // Set for an action-owned row; null for a row this table dispatches.
    QAction* action = nullptr;

    // Only meaningful for a table-dispatched row.
    Qt::Key key = Qt::Key_unknown;
    QString ownLabel;
    std::function<void()> invoke;

    // What to print. Note QAction::text() carries '&' mnemonics; strip them at
    // render time rather than storing a second copy of the string here.
    QList<QKeySequence> keys() const {
        if (action) return action->shortcuts();
        return QList<QKeySequence>{QKeySequence(key)};
    }
    QString label() const { return action ? action->text() : ownLabel; }
};

class ShortcutTable {
public:
    // A shortcut Qt dispatches for us. Listed so the table is the complete
    // keyboard contract rather than only the part one handler happens to own.
    void addAction(ShortcutGroup group, QAction* action);

    // A shortcut keyPressEvent dispatches.
    void addKey(ShortcutGroup group, Qt::Key key, QString label, std::function<void()> invoke);

    // Runs the handler for `event` if this table owns its key. Returns whether
    // it did, so the caller can fall through to the base class.
    //
    // MATCHES ON THE KEY ALONE AND IGNORES MODIFIERS, which is exactly what the
    // flat switch this replaced did -- Shift+Right stepped a frame, and still
    // does. That is deliberate rather than an oversight: every shortcut in Trace
    // that carries a modifier is already on a QAction (Ctrl+O, Ctrl+Return,
    // Alt+Enter), because QAction is what resolves modifier ambiguity properly.
    // The rule to keep is that a new modifier'd shortcut goes on an action; this
    // half of the table stays plain keys.
    bool dispatch(QKeyEvent* event) const;

    const std::vector<ShortcutRow>& rows() const noexcept { return rows_; }

private:
    std::vector<ShortcutRow> rows_;
};

} // namespace trace::app
