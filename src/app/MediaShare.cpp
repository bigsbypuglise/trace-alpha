#include "app/MediaShare.h"

#include "core/MediaIoSource.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#else
#include <QProcess>
#endif

namespace trace::app {

// One place that turns a path into the string every share command uses, and
// since spec phase 11 the string the recent list stores as well.
//
// canonicalFilePath() resolves symlinks and junctions, which is what "canonical"
// in the spec means -- but it returns EMPTY for a path that does not exist, so a
// file removed while open would silently produce an empty clipboard. Falling
// back to absoluteFilePath() keeps a usable answer in exactly that case, and the
// existence check is reported separately rather than folded into this.
QString canonicalNativePath(const QString& path) {
    const QFileInfo fi(path);
    QString resolved = fi.canonicalFilePath();
    if (resolved.isEmpty()) resolved = fi.absoluteFilePath();
    if (resolved.isEmpty()) return {};
    return QDir::toNativeSeparators(resolved);
}

const char* shareAvailabilityName(ShareAvailability state) {
    switch (state) {
        case ShareAvailability::Available:   return "ok";
        case ShareAvailability::Disabled:    return "disabled";
        case ShareAvailability::Unavailable: return "unavailable";
    }
    return "?";
}

ShareState evaluateShare(const QString& path, bool fileBacked,
                         const LucidIntegrationState& lucid) {
    ShareState state;
    state.fileBacked = fileBacked && !path.isEmpty();

    if (!state.fileBacked) {
        // The design package's exact wording for this case (section 11).
        const QString noFile = QObject::tr("This media has no file on disk.");
        state.copyPathReason = noFile;
        state.showInExplorerReason = noFile;
        state.lucidLinkReason = noFile;
        return state;
    }

    state.canonicalPath = canonicalNativePath(path);
    state.existedAtOpen = QFileInfo::exists(path);

    // REUSED, NOT REWRITTEN. MediaIoSource has classified volumes since the
    // LucidLink I/O work: it queries the volume rather than writing a probe
    // file, and it is cached per volume with a TTL, so asking it here costs one
    // lookup per media open. A second classifier would be a second answer to
    // the same question, free to disagree with the one the I/O path uses.
    const auto storage = trace::core::MediaIoSource::classifyStorage(path);
    state.onVirtualMount = storage.remote;
    state.mountReason = storage.reason;

    if (state.canonicalPath.isEmpty()) {
        const QString noPath = QObject::tr("This media has no usable file path.");
        state.copyPathReason = noPath;
        state.showInExplorerReason = noPath;
        state.lucidLinkReason = noPath;
        return state;
    }

    // Both ordinary commands are available for any file-backed source. Copy File
    // Path stays available even when the file has since been removed -- the path
    // is still the thing the user asked for, and it is what they would paste
    // into a bug report about the file being gone. Show in File Explorer cannot
    // do anything useful without the file, and says so.
    state.copyPath = ShareAvailability::Available;
    if (state.existedAtOpen) {
        state.showInExplorer = ShareAvailability::Available;
    } else {
        state.showInExplorer = ShareAvailability::Disabled;
        state.showInExplorerReason = QObject::tr("The file is no longer on disk.");
    }

    // --- the LucidLink gate --------------------------------------------------
    //
    // Three conditions, in the spec's own order: file-backed, on a LucidLink
    // filespace, integration available.
    //
    // THE CLASSIFIER ANSWERS A STORAGE-CLASS QUESTION, NOT A VENDOR ONE. It
    // recognises "virtual mount advertising petabyte capacity with free ==
    // total", which is true of any such mount and is a good NECESSARY condition
    // for LucidLink and a bad SUFFICIENT one. Treating it as sufficient would
    // reintroduce, one level up, exactly the "assume every V:\ path is
    // LucidLink" mistake the requirement exists to prevent -- so it can only
    // ever move this gate from Unavailable to Disabled, never to Available.
    //
    // WHAT MAKES IT AVAILABLE IS THE INTEGRATION'S OWN ANSWER, and phase 9
    // supplies it: `LucidIntegrationState::Supported` means the installed
    // extension was asked about THIS FILE and offered a link for it. Nothing
    // below infers support from the path, the drive letter or the volume shape.
    if (!state.onVirtualMount) {
        // Section 9's own example of Unavailable: a LucidLink link for a local
        // file. Nothing about this media will ever make it possible, and the
        // integration is not even consulted -- which is also what keeps COM and
        // a third-party DLL out of the common case entirely.
        state.lucidLink = ShareAvailability::Unavailable;
        state.lucidLinkReason =
            QObject::tr("This file is on a local volume, not a LucidLink filespace.");
        return state;
    }

    // Past here the volume is a virtual mount, so the file is ELIGIBLE and the
    // only question left is the integration. Every remaining branch is Disabled
    // rather than Unavailable, because what is missing is on this machine rather
    // than in the file -- except Supported.
    switch (lucid.status) {
        case LucidIntegrationState::Status::Supported:
            state.lucidLink = ShareAvailability::Available;
            break;
        case LucidIntegrationState::Status::Checking:
            state.lucidLink = ShareAvailability::Disabled;
            state.lucidLinkReason = QObject::tr("Checking the LucidLink integration...");
            break;
        case LucidIntegrationState::Status::NotInstalled:
        case LucidIntegrationState::Status::Unsupported:
            state.lucidLink = ShareAvailability::Disabled;
            state.lucidLinkReason = lucid.reason.isEmpty()
                ? QObject::tr("The LucidLink integration is not available.")
                : lucid.reason;
            break;
    }

    return state;
}

bool copyPathToClipboard(const ShareState& state, QString& error) {
    if (state.copyPath != ShareAvailability::Available) {
        error = state.copyPathReason;
        if (error.isEmpty()) error = QObject::tr("There is no file path to copy.");
        return false;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        error = QObject::tr("The clipboard is unavailable.");
        return false;
    }
    clipboard->setText(state.canonicalPath);
    return true;
}

void revealInFileExplorer(const ShareState& state, QObject* context,
                          std::function<void(const QString&)> onFailure) {
    // A QPointer, not the raw pointer. The callback is invoked from a pool
    // thread that can still be running SHOpenFolderAndSelectItems after the
    // window has been destroyed -- quitting the application while Explorer is
    // starting is an ordinary thing to do -- and posting to a dangling QObject
    // is undefined rather than merely useless. QPointer clears itself when the
    // object dies, so the late result is dropped instead.
    const QPointer<QObject> guard(context);
    const auto fail = [guard, onFailure](const QString& message) {
        if (!onFailure || guard.isNull()) return;
        QMetaObject::invokeMethod(guard, [onFailure, message]() { onFailure(message); },
                                  Qt::QueuedConnection);
    };

    if (state.showInExplorer != ShareAvailability::Available) {
        QString reason = state.showInExplorerReason;
        if (reason.isEmpty()) reason = QObject::tr("There is no file to show.");
        fail(reason);
        return;
    }

    const QString path = state.canonicalPath;

    // Off the UI thread. SHOpenFolderAndSelectItems starts or activates Explorer
    // and can take tens of milliseconds even warm, which during playback is a
    // dropped frame -- and priority 1 outranks this whole pass. QThreadPool is
    // used rather than a detached std::thread because it is joined at shutdown,
    // so a click immediately before Exit cannot leave a thread running through
    // teardown.
    class RevealTask final : public QRunnable {
    public:
        RevealTask(QString path, std::function<void(const QString&)> fail)
            : path_(std::move(path)), fail_(std::move(fail)) {
            setAutoDelete(true);
        }

        void run() override {
            // The file is re-checked HERE rather than at open time. Between
            // opening the media and clicking the menu item, the file can be
            // moved or a mount can drop; selecting a path that is gone makes
            // Explorer open some other folder, which looks like the wrong
            // command ran.
            if (!QFileInfo::exists(path_)) {
                fail_(QObject::tr("The file is no longer on disk."));
                return;
            }
#ifdef Q_OS_WIN
            // The shell interface, not `explorer.exe /select,<path>`. The
            // command-line form has no quoting that works for every path --
            // Explorer wants the path unquoted inside a single argument, which
            // is exactly what a process-argument quoter will not produce for a
            // path containing spaces -- and the spec's phase 9 rule ("invoke
            // through Windows shell interfaces rather than screen automation")
            // is the same house style one phase early.
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool owned = SUCCEEDED(init);
            PIDLIST_ABSOLUTE idl = nullptr;
            HRESULT hr = SHParseDisplayName(reinterpret_cast<PCWSTR>(path_.utf16()),
                                            nullptr, &idl, 0, nullptr);
            if (SUCCEEDED(hr) && idl) {
                hr = SHOpenFolderAndSelectItems(idl, 0, nullptr, 0);
                CoTaskMemFree(idl);
            }
            if (owned) CoUninitialize();
            if (FAILED(hr)) {
                fail_(QObject::tr("Windows could not show the file in File Explorer."));
            }
#else
            if (!QProcess::startDetached(QStringLiteral("xdg-open"),
                                         {QFileInfo(path_).absolutePath()})) {
                fail_(QObject::tr("Could not open the containing folder."));
            }
#endif
        }

    private:
        QString path_;
        std::function<void(const QString&)> fail_;
    };

    QThreadPool::globalInstance()->start(new RevealTask(path, fail));
}

} // namespace trace::app
