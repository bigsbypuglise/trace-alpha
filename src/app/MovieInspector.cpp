#include "app/MovieInspector.h"

#include <QClipboard>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

// THIS FILE CONTAINS NO QFile, QFileInfo OR QDir, AND NO DECODER, AND THAT IS
// THE POINT RATHER THAN AN OBSERVATION.
//
// Two of the spec's rules for the inspector are about what it must NOT do:
// "do not continuously poll expensive decoder state" and "do not block
// remote-media opening to calculate optional values". The second one is phase
// 11's trap wearing a different costume -- an unreachable UNC path costs
// 21,037ms to stat on this box and a cold LucidLink read measured 407ms, so a
// `File size` row that stats when the window is shown is a 21-second freeze on
// exactly the storage this tool exists to review from.
//
// RecentFiles.cpp enforced its own refusal by containing none of those three
// classes, so a later change that wants to probe has to add an include first --
// a visible act rather than a one-line slip. The phase handoff predicted this
// file could not do the same "since it must report a size". It can: the size is
// not computed here. Everything below arrives already computed, in a value
// type, from state the application read at open or already maintains. This
// window has no pointer to the decoder, no pointer to the viewer and no path it
// could stat, so it cannot poll and it cannot block, and neither refusal
// depends on anyone remembering.

namespace trace::app {
namespace {

QString originTag(FieldOrigin origin) {
    switch (origin) {
        case FieldOrigin::File: return MovieInspector::tr("file");
        case FieldOrigin::Encoded: return MovieInspector::tr("encoded");
        case FieldOrigin::Observed: return MovieInspector::tr("observed");
        case FieldOrigin::Playback: return MovieInspector::tr("playback");
    }
    return {};
}

} // namespace

MovieInspector::MovieInspector(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Movie Inspector"));
    // MODELESS. The spec says "non-blocking", and a QDialog is modeless unless
    // exec() is called -- which nothing here does. Qt::Window rather than the
    // default dialog hint so it gets a real minimise/maximise-capable frame and
    // reads as a panel the user can leave open, not as a prompt.
    setWindowFlag(Qt::Window, true);
    // Deleting it on close would take the snapshot and the collapsed-section
    // set with it, so reopening would forget which sections the user had shut.
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(560, 700);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    // NO HORIZONTAL SCROLLING, AND THAT IS A CORRECTNESS SETTING RATHER THAN A
    // COSMETIC ONE. A source path is one long token, so a QLabel holding it asks
    // the layout for a very wide minimum; with a horizontal scrollbar available
    // the layout grants it, the grid grows past the window, and the ORIGIN
    // COLUMN -- the whole "which of these is the file's claim" answer -- ends up
    // off-screen. Measured on the first build: every `encoded`/`observed` tag was
    // outside the viewport on a 536px window. Refusing the scrollbar forces the
    // value column to wrap instead, which is what the size policy below allows.
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_ = new QWidget(scroll_);
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(12, 12, 12, 12);
    contentLayout_->setSpacing(10);
    scroll_->setWidget(content_);
    root->addWidget(scroll_, 1);

    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 10);

    // The legend, because four origin tags with no key is a puzzle. It is one
    // line and it is the sentence the whole window is arranged around.
    auto* legend = new QLabel(
        tr("<b>encoded</b> — what the file states · <b>file</b> — the file on disk · "
           "<b>observed</b> — this window now · <b>playback</b> — what Trace does about it"),
        footer);
    legend->setWordWrap(true);
    QFont legendFont = legend->font();
    legendFont.setPointSizeF(std::max(7.0, legendFont.pointSizeF() - 1.0));
    legend->setFont(legendFont);
    footerLayout->addWidget(legend, 1);

    // "Long paths must be copyable" (spec). Every value label below is
    // selectable, which covers it; this covers the case where the user wants
    // the whole report to paste into a bug.
    auto* copyButton = new QPushButton(tr("Copy All"), footer);
    copyButton->setAutoDefault(false);
    copyButton->setDefault(false);
    connect(copyButton, &QPushButton::clicked, this, &MovieInspector::copyAll);
    footerLayout->addWidget(copyButton, 0);

    root->addWidget(footer, 0);

    rebuild();
}

void MovieInspector::setSnapshot(const InspectorSnapshot& snapshot) {
    snapshot_ = snapshot;
    rebuild();
}

void MovieInspector::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    emit visibilityChanged(true);
}

void MovieInspector::hideEvent(QHideEvent* event) {
    QDialog::hideEvent(event);
    emit visibilityChanged(false);
}

void MovieInspector::rebuild() {
    // Wholesale, because which sections exist depends on the media: a file with
    // no audio track has no audio section, and a still image has no video one.
    // The collapsed set is what survives, so a rebuild triggered by a resize
    // cannot re-open a section the user shut.
    //
    // Rebuilds are rare by construction -- see MainWindow's refresh timer. This
    // is not called from a paint, a resize event or a timeline update.
    while (QLayoutItem* item = contentLayout_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    setWindowTitle(snapshot_.fileName.isEmpty()
                       ? tr("Movie Inspector")
                       : tr("Movie Inspector — %1").arg(snapshot_.fileName));

    if (snapshot_.sections.empty()) {
        auto* empty = new QLabel(tr("No media open."), content_);
        empty->setEnabled(false);
        contentLayout_->addWidget(empty);
        contentLayout_->addStretch(1);
        return;
    }

    for (const InspectorSection& section : snapshot_.sections) {
        // A checkable QToolButton with an arrow indicator is the collapsible
        // section: no animation, no custom painting, and it is a real focusable
        // widget, which matters because the floating transport has no widget
        // tree at all and every accessible surface Trace has is a Qt one.
        auto* header = new QToolButton(content_);
        header->setText(section.title);
        header->setCheckable(true);
        header->setChecked(!collapsed_.contains(section.title));
        header->setArrowType(header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setAutoRaise(true);
        QFont headerFont = header->font();
        headerFont.setBold(true);
        header->setFont(headerFont);
        contentLayout_->addWidget(header);

        auto* body = new QWidget(content_);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(18, 2, 0, 6);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        // The origin column is reserved rather than left to compete for space.
        // It is the shortest column and the first one a greedy value would push
        // out, and it is the one carrying the answer the window exists to give.
        grid->setColumnMinimumWidth(2, 58);

        int row = 0;
        for (const InspectorField& field : section.fields) {
            auto* name = new QLabel(field.label + QStringLiteral(":"), body);
            name->setAlignment(Qt::AlignLeft | Qt::AlignTop);

            QWidget* value = nullptr;
            if (field.entry) {
                auto* edit = new QLineEdit(field.value, body);
                edit->setReadOnly(true);
                edit->setFrame(false);
                edit->setCursorPosition(0);
                // A read-only QLineEdit still takes focus, which is what makes
                // Ctrl+A / Ctrl+C work on it -- and it is why the phase 7
                // shortcut guard matters here: with the panel focused, typing a
                // bound single key must not reach the transport. It cannot: this
                // is a separate top-level window and QLineEdit consumes
                // printable keys through QEvent::ShortcutOverride.
                edit->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; }"));
                value = edit;
            } else {
                auto* text = new QLabel(field.value, body);
                text->setWordWrap(true);
                // Preferred, with height-for-width left ON. The second build set
                // this to Ignored to stop long values widening the grid, and
                // that cost the label its wrapped height: the source path
                // rendered as "C:" and the playback row lost its second line.
                // Every value that wraps contains spaces, so its minimum size
                // hint is a short word; the ONE value that does not is the path,
                // and that is what `entry` is for.
                QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
                policy.setHeightForWidth(true);
                text->setSizePolicy(policy);
                text->setMinimumWidth(160);
                // Selectable, so any value can be copied out of the window.
                text->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                              Qt::TextSelectableByKeyboard);
                value = text;
            }

            auto* tag = new QLabel(originTag(field.origin), body);
            tag->setAlignment(Qt::AlignRight | Qt::AlignTop);
            tag->setEnabled(false);
            QFont tagFont = tag->font();
            tagFont.setPointSizeF(std::max(7.0, tagFont.pointSizeF() - 1.0));
            tag->setFont(tagFont);

            grid->addWidget(name, row, 0);
            grid->addWidget(value, row, 1);
            grid->addWidget(tag, row, 2);
            ++row;
        }

        body->setVisible(header->isChecked());
        contentLayout_->addWidget(body);

        const QString title = section.title;
        connect(header, &QToolButton::toggled, this, [this, header, body, title](bool on) {
            body->setVisible(on);
            header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            if (on) collapsed_.remove(title);
            else collapsed_.insert(title);
        });
    }

    contentLayout_->addStretch(1);
}

void MovieInspector::copyAll() const {
    QString text;
    if (!snapshot_.fileName.isEmpty()) {
        text += snapshot_.fileName + QStringLiteral("\n\n");
    }
    for (const InspectorSection& section : snapshot_.sections) {
        text += section.title + QStringLiteral("\n");
        for (const InspectorField& field : section.fields) {
            // The origin travels with the value. A pasted report that says
            // "Matrix coefficients: Untagged" and "Playback matrix: bt709" is
            // only readable if it also says which of the two the file stated.
            text += QStringLiteral("  %1: %2  [%3]\n")
                        .arg(field.label, field.value, originTag(field.origin));
        }
        text += QStringLiteral("\n");
    }
    QGuiApplication::clipboard()->setText(text);
}

} // namespace trace::app
