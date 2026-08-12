#pragma once

#include <QDialog>

namespace trace::app {

class ShortcutTable;

// The Keyboard Shortcuts window (spec phase 14, Help menu).
//
// IT IS GENERATED, NOT WRITTEN. Every row comes from ShortcutTable::rows(),
// which is why phase 3 replaced keyPressEvent's flat switch with a table long
// before there was a window to render it into: a switch cannot be enumerated,
// grouped or printed, so the only alternative was a second hand-maintained list
// of the same keys, and the two would have disagreed the first time a binding
// moved.
//
// Rows that carry a QAction ask the action for its keys and its text at RENDER
// time, so a rebinding shows up here without anyone remembering to update it.
// Rows the table dispatches itself carry their own key and label. Neither kind
// is duplicated in this file.
//
// Modeless, like the Movie Inspector: a reference card is something to leave
// open beside the picture, not something to dismiss before continuing.
class ShortcutsWindow final : public QDialog {
    Q_OBJECT
public:
    // Takes the table by reference and reads it ONCE, here. The table is owned
    // by MainWindow and outlives this window; nothing is copied out of it into
    // a second structure that could go stale.
    explicit ShortcutsWindow(const ShortcutTable& table, QWidget* parent = nullptr);
};

} // namespace trace::app
