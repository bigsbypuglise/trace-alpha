#pragma once

#include <QString>
#include <QStringList>

namespace trace::app {

// The bounded recent-file list (spec phase 11).
//
// THIS CLASS NEVER TOUCHES THE FILESYSTEM, AND THAT IS ITS ENTIRE DESIGN. The
// spec's rules for Open Recent are mostly refusals -- do not probe every path
// during application startup, do not block on disconnected LucidLink/network
// paths -- and the way to make a refusal hold is to give the module no way to
// break it. There is no QFile, no QFileInfo and no QDir in RecentFiles.cpp:
// entries are strings in, strings out.
//
// The cost of getting that wrong is not theoretical. A cold LucidLink read
// measured 407ms in the storage work, and an unroutable UNC path costs a
// multi-second SMB connect timeout. A list that stat()ed its entries to grey
// out the missing ones would hang the File menu on a disconnected mount -- and
// it would do it at the exact moment the user reached for the menu, which reads
// as the application freezing rather than as a network being down.
//
// So the menu is drawn from stored strings and every entry is always enabled.
// Whether a file is still there is discovered by TRYING TO OPEN IT, which is
// work the user just asked for, at a cost they would have paid through
// File > Open anyway.
//
// Paths stored here are canonical (MediaShare::canonicalNativePath), which the
// spec requires and which is also what makes de-duplication mean anything --
// the same file reached through a junction and through its real path is one
// entry, not two.
class RecentFiles {
public:
    // Bounded, and deliberately short. The spec asks for a bound, and it also
    // asks not to log sensitive path history unnecessarily; a long list is a
    // longer record of what someone has been reviewing, kept in a plain-text
    // file, for no proportionate gain.
    static constexpr int kMaxEntries = 10;

    // Reads the stored strings. No validation against the filesystem, by
    // design; see the class comment.
    void load();

    const QStringList& paths() const { return paths_; }
    bool isEmpty() const { return paths_.isEmpty(); }

    // Most-recent-first insert with de-duplication, bounded, persisted.
    // Case-insensitive matching, because Windows paths are.
    void remember(const QString& canonicalPath);

    // Removes one entry. Persisted. This is what the "file is missing -- remove
    // it?" prompt calls; the spec asks for the entry to be REPORTED and offered
    // for removal rather than silently dropped, so nothing else calls it.
    void forget(const QString& canonicalPath);

    // Clear Recent Files.
    void clear();

private:
    void save() const;

    QStringList paths_;
};

} // namespace trace::app
