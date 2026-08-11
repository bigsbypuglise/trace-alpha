#pragma once

#include <QDialog>
#include <QSet>
#include <QString>
#include <vector>

QT_BEGIN_NAMESPACE
class QScrollArea;
class QVBoxLayout;
QT_END_NAMESPACE

namespace trace::app {

// WHERE A VALUE CAME FROM, WHICH IS HALF OF WHAT THE INSPECTOR IS FOR.
//
// The spec's rules for this window are "display Unknown or Untagged honestly;
// do not infer missing colour metadata inside the inspector; distinguish
// encoded metadata from playback inference". The metadata layer (phase 13's
// first commit) made the third one possible for the four colour fields by
// keeping the container's tags apart from the matrix playback chose. But the
// same line runs through the rest of the panel: `Display aspect ratio` is the
// file's claim and `Current scale` is this window's state, and a user reading
// one under the other should not have to know which is which.
//
// So every row carries its origin and the window prints it. Four, not two,
// because "the file states 16:9" and "the file on disk is 2.3 GB" and "this
// window is showing it at 38%" and "playback picked BT.709 because nothing
// said" are four different kinds of claim.
enum class FieldOrigin {
    // A property of the file on disk: its name, its path, its size.
    File,
    // Stated by the container or the stream. What the file SAYS.
    Encoded,
    // This session, this window, right now. Changes with no change to the file.
    Observed,
    // What Trace decided to do about what the file said -- including what it
    // decided when the file said nothing. Never presented as the file's claim.
    Playback,
};

struct InspectorField {
    QString label;
    QString value;
    FieldOrigin origin = FieldOrigin::Encoded;
    // Render in a read-only entry rather than a wrapping label.
    //
    // For ONE value: the source path. A path is a single unbroken token, so a
    // word-wrapping QLabel holding it demands a very wide minimum from the
    // layout -- which pushed the origin column off-screen on the first build --
    // and constraining the label instead cost it its height-for-width and
    // clipped the path to "C:" on the second. An entry has a small minimum
    // width, scrolls its own text, and is selectable and copyable in full,
    // which is what the spec asks of long paths.
    bool entry = false;
};

struct InspectorSection {
    QString title;
    std::vector<InspectorField> fields;
};

// Everything the window shows, already computed.
//
// It is a value rather than a pointer to anything because that is what makes
// "the dialog reads, it does not ask" a property instead of a promise -- see
// the note at the top of MovieInspector.cpp.
struct InspectorSnapshot {
    QString fileName;
    // Shown selectably and used by Copy All. Empty when nothing is open.
    QString sourcePath;
    std::vector<InspectorSection> sections;
};

// The Movie Inspector (spec phase 13). Modeless -- the spec says "non-blocking"
// -- with collapsible sections.
class MovieInspector final : public QDialog {
    Q_OBJECT
public:
    explicit MovieInspector(QWidget* parent = nullptr);

    void setSnapshot(const InspectorSnapshot& snapshot);

signals:
    // Emitted when the window is shown or hidden by ANY route, including the
    // title bar's close button. The menu item is checkable, so it has to follow
    // the window rather than assume it drove every change.
    void visibilityChanged(bool visible);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void rebuild();
    void copyAll() const;

    InspectorSnapshot snapshot_;
    QWidget* content_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    // Section titles the user has collapsed. Kept across rebuilds -- a refresh
    // triggered by a resize must not silently re-open a section the user shut,
    // and the sections are rebuilt wholesale because which ones exist depends on
    // the media (a file with no audio track has no audio section).
    QSet<QString> collapsed_;
};

} // namespace trace::app
