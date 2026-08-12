#include "app/ShortcutsWindow.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <map>
#include <vector>

#include "app/ShortcutTable.h"

namespace trace::app {
namespace {

QString groupTitle(ShortcutGroup group) {
    switch (group) {
        case ShortcutGroup::File: return ShortcutsWindow::tr("File");
        case ShortcutGroup::Playback: return ShortcutsWindow::tr("Playback");
        case ShortcutGroup::Stepping: return ShortcutsWindow::tr("Stepping");
        case ShortcutGroup::Shuttle: return ShortcutsWindow::tr("Shuttle");
        case ShortcutGroup::View: return ShortcutsWindow::tr("View");
    }
    return {};
}

// The order groups print in. Explicit rather than the enum's declaration order,
// because the reading order of a reference card is a presentation decision and
// the enum's order is not -- and because a group added to the enum later would
// otherwise silently land wherever it was declared.
constexpr ShortcutGroup kGroupOrder[] = {
    ShortcutGroup::File,      ShortcutGroup::Playback, ShortcutGroup::Stepping,
    ShortcutGroup::Shuttle,   ShortcutGroup::View,
};

// QAction::text() carries '&' mnemonics, which are for the menu and are noise
// on a reference card. Stripped at RENDER time rather than stored a second
// time -- ShortcutRow::label() deliberately returns the action's own string, so
// that the row cannot hold a copy that drifts.
QString displayLabel(const ShortcutRow& row) {
    QString text = row.label();
    text.remove(QLatin1Char('&'));
    // Menu items often end in "..." to say they open a dialog. On a key list
    // the ellipsis says nothing the key does not.
    if (text.endsWith(QStringLiteral("..."))) text.chop(3);
    return text.trimmed();
}

// Every sequence a row answers to, joined. A row can carry several -- the
// diagnostics HUD is H, Return and Enter, and fullscreen is F11, Ctrl+Return
// and Alt+Enter -- and printing only the first would make this window disagree
// with the application about what works.
//
// NativeText, not PortableText: this window is read by someone at a Windows
// keyboard, so "Ctrl+Return" should say what is written on the key.
QString displayKeys(const ShortcutRow& row) {
    QStringList parts;
    const auto sequences = row.keys();
    parts.reserve(sequences.size());
    for (const auto& sequence : sequences) {
        const QString text = sequence.toString(QKeySequence::NativeText);
        if (!text.isEmpty()) parts << text;
    }
    return parts.join(QStringLiteral(" · "));  // middle dot
}

} // namespace

ShortcutsWindow::ShortcutsWindow(const ShortcutTable& table, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    // Modeless, and it keeps its own minimise/close chrome so it can be left
    // open beside the picture.
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setSizeGripEnabled(true);

    auto* outer = new QVBoxLayout(this);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 12, 4);

    // Bucket the rows by group, PRESERVING the order they were registered in
    // within each group. That order is the order setupShortcuts() adds them,
    // which reads J-K-L in sequence and Left-then-Right for stepping -- an
    // alphabetical or key-code sort would scatter both.
    std::map<ShortcutGroup, std::vector<const ShortcutRow*>> byGroup;
    for (const auto& row : table.rows()) byGroup[row.group].push_back(&row);

    for (const auto group : kGroupOrder) {
        const auto it = byGroup.find(group);
        if (it == byGroup.end() || it->second.empty()) continue;

        auto* heading = new QLabel(groupTitle(group), content);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
        layout->addSpacing(layout->count() ? 10 : 0);
        layout->addWidget(heading);

        auto* grid = new QGridLayout;
        grid->setContentsMargins(8, 2, 0, 0);
        grid->setHorizontalSpacing(24);
        grid->setVerticalSpacing(4);
        // The key column is fixed and the description takes the slack, so the
        // keys line up down the page and a long description wraps rather than
        // widening the window.
        grid->setColumnStretch(0, 0);
        grid->setColumnStretch(1, 1);

        int rowIndex = 0;
        for (const auto* row : it->second) {
            const QString keys = displayKeys(*row);
            // A row with no key at all is not a keyboard shortcut and does not
            // belong on a keyboard reference. The table already excludes the
            // three shortcut-less view transforms for this reason; this is the
            // check that keeps it true if one is ever added.
            if (keys.isEmpty()) continue;

            auto* keyLabel = new QLabel(keys, content);
            keyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            QFont keyFont = keyLabel->font();
            keyFont.setBold(true);
            keyLabel->setFont(keyFont);
            keyLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);

            auto* textLabel = new QLabel(displayLabel(*row), content);
            textLabel->setWordWrap(true);

            grid->addWidget(keyLabel, rowIndex, 0);
            grid->addWidget(textLabel, rowIndex, 1);
            ++rowIndex;
        }
        layout->addLayout(grid);
    }

    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // WHY THIS NOTE IS HERE AND NOT IN A COMMENT ONLY: the shuttle ladders are
    // the one place where the key and the button deliberately disagree, and a
    // reference card that printed "Rewind - 1x, 2x, 5x, 10x, 30x" without it
    // would make the on-screen buttons look broken to anyone who read it.
    auto* note = new QLabel(
        tr("<small>The Rewind and Fast-forward <b>buttons</b> enter the ladder at "
           "2x. <b>J</b> and <b>L</b> enter at 1x, so a single <b>L</b> is "
           "ordinary playback.</small>"),
        this);
    note->setWordWrap(true);
    outer->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    outer->addWidget(buttons);

    resize(460, 560);
}

} // namespace trace::app
