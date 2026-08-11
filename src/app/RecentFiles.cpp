#include "app/RecentFiles.h"

#include "app/Settings.h"

#include <QSettings>

// NO <QFile>, <QFileInfo> OR <QDir> HERE, ON PURPOSE. See RecentFiles.h: the
// list must not be able to touch the filesystem, and the cheapest way to
// guarantee that is for the code to have no way to.

namespace trace::app {

namespace {

constexpr auto kArrayKey = "recentFiles";
constexpr auto kPathKey = "path";

// Windows paths are case-insensitive, so `C:\Clips\a.mov` and `c:\clips\A.MOV`
// are one entry. Canonicalisation upstream already resolves junctions and
// relative segments; it does NOT normalise case, and two entries for one file
// is exactly the confusion a recent list exists to avoid.
bool samePath(const QString& a, const QString& b) {
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

void RecentFiles::load() {
    paths_.clear();
    QSettings& s = settings();

    // A numbered array rather than a single QStringList value. QSettings'
    // IniFormat encodes a string list as one comma-separated line, which round
    // trips through Qt's own escaping but is opaque in a file a user may open,
    // and a one-element list reads back as a plain QString. An array is
    // unambiguous in both directions.
    const int count = s.beginReadArray(kArrayKey);
    for (int i = 0; i < count && paths_.size() < kMaxEntries; ++i) {
        s.setArrayIndex(i);
        const QString path = s.value(kPathKey).toString();
        if (path.isEmpty()) continue;
        // De-duplicate on the way in as well as on insert. A file edited by
        // hand, or written by an older build, must not be able to produce two
        // rows that look identical and behave identically.
        bool seen = false;
        for (const QString& existing : paths_) {
            if (samePath(existing, path)) { seen = true; break; }
        }
        if (!seen) paths_.push_back(path);
    }
    s.endArray();
}

void RecentFiles::remember(const QString& canonicalPath) {
    if (canonicalPath.isEmpty()) return;

    for (int i = paths_.size() - 1; i >= 0; --i) {
        if (samePath(paths_.at(i), canonicalPath)) paths_.removeAt(i);
    }
    paths_.push_front(canonicalPath);
    while (paths_.size() > kMaxEntries) paths_.removeLast();
    save();
}

void RecentFiles::forget(const QString& canonicalPath) {
    if (canonicalPath.isEmpty()) return;
    bool removed = false;
    for (int i = paths_.size() - 1; i >= 0; --i) {
        if (samePath(paths_.at(i), canonicalPath)) {
            paths_.removeAt(i);
            removed = true;
        }
    }
    if (removed) save();
}

void RecentFiles::clear() {
    if (paths_.isEmpty()) return;
    paths_.clear();
    save();
}

void RecentFiles::save() const {
    QSettings& s = settings();
    // remove() first: beginWriteArray leaves any indices past the new size in
    // place, so a list that shrinks would keep its tail and Clear Recent Files
    // would clear nothing at all.
    s.remove(kArrayKey);
    s.beginWriteArray(kArrayKey, paths_.size());
    for (int i = 0; i < paths_.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(kPathKey, paths_.at(i));
    }
    s.endArray();
    // Written through immediately. A recent list that survives a clean quit but
    // not a crash is a recent list that will be reported as forgetting things at
    // random, and there is one of these writes per media open -- not per frame.
    syncSettings();
}

} // namespace trace::app
