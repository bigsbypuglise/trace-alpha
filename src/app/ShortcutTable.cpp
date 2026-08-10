#include "app/ShortcutTable.h"

#include <QKeyEvent>

namespace trace::app {

void ShortcutTable::addAction(ShortcutGroup group, QAction* action) {
    if (!action) return;
    ShortcutRow row;
    row.group = group;
    row.action = action;
    rows_.push_back(std::move(row));
}

void ShortcutTable::addKey(ShortcutGroup group,
                           Qt::Key key,
                           QString label,
                           std::function<void()> invoke) {
    ShortcutRow row;
    row.group = group;
    row.key = key;
    row.ownLabel = std::move(label);
    row.invoke = std::move(invoke);
    rows_.push_back(std::move(row));
}

bool ShortcutTable::dispatch(QKeyEvent* event) const {
    if (!event) return false;
    const int pressed = event->key();
    for (const ShortcutRow& row : rows_) {
        // Action-owned rows are documentation here and nothing else: Qt has
        // already run them, so dispatching one would run it twice.
        if (!row.invoke || row.key == Qt::Key_unknown) continue;
        if (pressed != static_cast<int>(row.key)) continue;
        row.invoke();
        return true;
    }
    return false;
}

} // namespace trace::app
