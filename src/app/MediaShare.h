#pragma once

#include <QString>
#include <functional>

QT_BEGIN_NAMESPACE
class QObject;
QT_END_NAMESPACE

namespace trace::app {

// Whether a share command can run, and why not when it cannot.
//
// The design package (section 9, State Semantics) separates two states that a
// single `bool enabled` would conflate, and the difference is visible to the
// user because it decides what the tooltip says:
//
//   Disabled     the action exists and could run, but not right now.
//   Unavailable  THIS MEDIA can never support it -- a LucidLink link for a file
//                on a local disk. Rendered the same way, plus a tooltip stating
//                why, and the row is NEVER hidden.
//
// Hiding the row was the tempting simplification and is explicitly forbidden: a
// menu whose items come and go cannot be learned, and "the command is missing"
// reads as a broken build rather than as an answer about the file.
enum class ShareAvailability { Available, Disabled, Unavailable };

// For the diagnostics HUD. The three states have to be distinguishable in a
// capture, because "is that menu item greyed out" is not answerable from a
// screenshot at 15px -- the first attempt to verify this gate measured icon
// luminance and could not separate an Unavailable row from a shorter label.
const char* shareAvailabilityName(ShareAvailability state);

// What the three Share commands are allowed to do for the current media.
// Computed ONCE per open, never per paint or per timeline update -- the spec's
// performance requirements forbid filesystem probing in either, and
// classifyStorage() below issues real volume queries.
struct ShareState {
    // Is there a file on disk at all? False for a non-file source, which is the
    // case both path commands have to decline.
    bool fileBacked = false;
    // Canonical, native-separator Windows path. This is what Copy File Path
    // puts on the clipboard and what Show in File Explorer selects.
    QString canonicalPath;
    // Re-checked when a command runs rather than trusted from open time: a file
    // can be removed or a mount can drop while it is open, and the spec asks
    // both commands to handle that cleanly.
    bool existedAtOpen = false;

    // The storage-class answer from MediaIoSource::classifyStorage, carried
    // separately from the LucidLink verdict below BECAUSE THEY ARE NOT THE SAME
    // QUESTION. See lucidLinkGate().
    bool onVirtualMount = false;
    QString mountReason;

    ShareAvailability copyPath = ShareAvailability::Unavailable;
    QString copyPathReason;
    ShareAvailability showInExplorer = ShareAvailability::Unavailable;
    QString showInExplorerReason;
    ShareAvailability lucidLink = ShareAvailability::Unavailable;
    QString lucidLinkReason;
};

// Whether the installed LucidLink integration can be driven right now.
//
// SPEC PHASE 9 IMPLEMENTS THIS AND PHASE 8 DOES NOT. It returns false today,
// with a reason that says only what has actually been established -- that this
// build has no integration -- rather than the design package's "LucidLink is not
// running", which asserts a cause nothing here has checked. Phase 9 replaces the
// body and the string together.
//
// It is a named function rather than a `false` at the call site so the gate
// below reads as three conditions now and still reads as three conditions when
// one of them starts returning true.
bool lucidLinkIntegrationAvailable(QString& reason);

// The whole gate, in one place, for one media item.
//
// `fileBacked` is the caller's answer to "is this media a file on disk", which
// is a question about MediaItem rather than about the filesystem, so it is
// passed in rather than inferred from the path.
ShareState evaluateShare(const QString& path, bool fileBacked);

// Puts the canonical path on the clipboard. Returns false and fills `error`
// when the state says it must not run, so a caller cannot copy a path the gate
// declined by forgetting to check.
bool copyPathToClipboard(const ShareState& state, QString& error);

// Opens the containing folder with the file selected.
//
// ASYNCHRONOUS, and that is a requirement rather than a convenience: the spec's
// performance list says no blocking shell calls on the UI thread, and this is a
// shell call that starts a process. `onFailure` is invoked on `context`'s
// thread, so the caller may touch the status bar from it directly.
//
// It does not start playback and does not copy or move the file.
void revealInFileExplorer(const ShareState& state, QObject* context,
                          std::function<void(const QString&)> onFailure);

} // namespace trace::app
