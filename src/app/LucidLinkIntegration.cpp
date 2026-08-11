#include "app/LucidLinkIntegration.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QSettings>
#include <QThread>

#ifdef Q_OS_WIN

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace trace::app {
namespace {

// The command's DISPLAY STRING, and the version/localization risk it carries.
//
// The extension exposes NO canonical verb: GetCommandString(GCS_VERBW) fails for
// every item it contributes, so there is nothing stable to ask for by name and
// the item has to be recognised by the text it shows. The spec anticipates this
// ("if only a display string is exposed, account for version/localization risk
// and document it before shipping") and this is that documentation:
//
//   - measured against LucidShellExt 1.0.15, which renders "Copy link";
//   - a localized Windows would render a translated string and Trace would
//     report the integration as unavailable rather than invoking the wrong item;
//   - failing CLOSED is the whole reason this is a strict match. The item
//     immediately above "Copy link" is "Pin", which HYDRATES THE FILE ONTO THE
//     MOUNT. Positional or fuzzy identification could invoke it, and `V:\` is
//     live client production storage. So: exact text, or nothing runs.
constexpr wchar_t kCopyLinkText[] = L"Copy link";

// Where Explorer registers per-file context-menu handlers. Read in this order;
// the first that yields a working handler wins.
const wchar_t* const kHandlerRoots[] = {
    L"SOFTWARE\\Classes\\AllFilesystemObjects\\shellex\\ContextMenuHandlers",
    L"SOFTWARE\\Classes\\*\\shellex\\ContextMenuHandlers",
};

QString regString(HKEY root, const QString& subKey, const QString& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, reinterpret_cast<const wchar_t*>(subKey.utf16()), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    const LONG r = RegQueryValueExW(key, value.isEmpty() ? nullptr
                                                         : reinterpret_cast<const wchar_t*>(value.utf16()),
                                    nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return QString::fromWCharArray(buf).trimmed();
}

// Strips the menu mnemonics and whitespace Windows puts around item text.
QString plainMenuText(const wchar_t* raw) {
    QString s = QString::fromWCharArray(raw);
    s.remove(QLatin1Char('&'));
    return s.trimmed();
}

// One RAII apartment, initialised the way EXPLORER initialises it.
//
// `OleInitialize` rather than `CoInitializeEx`: the handler registers
// ThreadingModel=Apartment so an STA is required either way, and a shell
// extension handed an `IDataObject` is entitled to assume the full OLE stack --
// clipboard and drag-drop -- is up, which `CoInitializeEx` alone does not start.
// That is a PRECAUTION, not a fix, and the distinction is recorded because it
// was very nearly written down as a fix.
//
// THE STORY, because it is the sharper lesson of this phase. The first build
// reported `lucid disabled` on the nominated file. Switching to `OleInitialize`
// made it read `lucid ok`, and "the extension needs OLE" was an appealing
// explanation. It is wrong: that build ALSO failed to refresh the HUD after the
// probe landed, so the reading was a stale INSTRUMENT and never said anything
// about the probe at all. The menu -- which reads live state -- was already
// correct.
//
// `TRACE_LUCID_COINIT=1` is the control that settles it, and it is retained the
// way `TRACE_LONGGOP_SLICE_THREADS` is: a closed question keeps its control.
// Measured on the nominated file, both apartments read
// `Initialize -> 0x00000000` and `SUPPORTED via LucidContextMenu (offset 2)`.
class ShellApartment {
public:
    ShellApartment() {
        useOle_ = qgetenv("TRACE_LUCID_COINIT") != "1";
        owned_ = useOle_ ? SUCCEEDED(OleInitialize(nullptr))
                         : SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }
    ~ShellApartment() {
        if (!owned_) return;
        if (useOle_) OleUninitialize(); else CoUninitialize();
    }
    ShellApartment(const ShellApartment&) = delete;
    ShellApartment& operator=(const ShellApartment&) = delete;
    bool ok() const { return owned_; }
    bool usedOle() const { return useOle_; }
private:
    bool owned_ = false;
    bool useOle_ = true;
};

// One line per probe/copy when TRACE_LUCID_LOG=1, on stderr.
//
// The gate is three states deep and every one of them is a refusal with a
// reason; without this, "disabled" in the HUD is the only symptom and it looks
// identical whether the handler was not found, the extension declined the file,
// or the menu had no matching item.
bool lucidLogging() {
    static const bool on = !qEnvironmentVariableIsEmpty("TRACE_LUCID_LOG")
                        && qgetenv("TRACE_LUCID_LOG") != "0";
    return on;
}
void lucidLog(const QString& line) {
    if (!lucidLogging()) return;
    fprintf(stderr, "[lucid] %s\n", line.toLocal8Bit().constData());
    fflush(stderr);
}

// Builds the extension's own menu for one file and reports the command offset of
// the copy-link item, or -1.
//
// ONLY THE NAMED HANDLER IS CREATED. Going through the merged Explorer context
// menu would load every registered handler into Trace's process -- on the test
// box that is Adobe, OneDrive, PowerToys, Tailscale and Copilot -- which is not
// something a media player should do to itself, and it would also put "Copy
// link" in a menu whose composition Trace does not control. Creating the one
// CLSID yields a menu containing only LucidLink's commands.
struct HandlerMenu {
    IContextMenu* menu = nullptr;
    HMENU hmenu = nullptr;
    int copyLinkOffset = -1;
    HRESULT initHr = E_FAIL;

    ~HandlerMenu() {
        if (hmenu) DestroyMenu(hmenu);
        if (menu) menu->Release();
    }
};

bool buildHandlerMenu(const LucidHandler& handler, const QString& path, HandlerMenu& out) {
    CLSID clsid{};
    if (FAILED(CLSIDFromString(reinterpret_cast<LPCOLESTR>(handler.clsid.utf16()), &clsid))) {
        return false;
    }

    const QString native = QDir::toNativeSeparators(path);
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(reinterpret_cast<PCWSTR>(native.utf16()), nullptr,
                                  &pidl, 0, nullptr)) || !pidl) {
        return false;
    }

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child);
    if (FAILED(hr) || !parent) { CoTaskMemFree(pidl); return false; }

    // The selection the extension is asked about, expressed the way Explorer
    // expresses it.
    IDataObject* data = nullptr;
    hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr,
                               reinterpret_cast<void**>(&data));
    parent->Release();
    CoTaskMemFree(pidl);
    if (FAILED(hr) || !data) return false;

    IShellExtInit* init = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IShellExtInit,
                          reinterpret_cast<void**>(&init));
    if (FAILED(hr) || !init) { data->Release(); return false; }

    out.initHr = init->Initialize(nullptr, data, nullptr);
    data->Release();
    lucidLog(QStringLiteral("  %1 Initialize -> 0x%2")
                 .arg(handler.keyName)
                 .arg(static_cast<quint32>(out.initHr), 8, 16, QLatin1Char('0')));
    if (FAILED(out.initHr)) { init->Release(); return false; }

    hr = init->QueryInterface(IID_IContextMenu, reinterpret_cast<void**>(&out.menu));
    init->Release();
    if (FAILED(hr) || !out.menu) return false;

    out.hmenu = CreatePopupMenu();
    if (!out.hmenu) return false;

    constexpr UINT kIdFirst = 1;
    constexpr UINT kIdLast = 0x7FFF;
    hr = out.menu->QueryContextMenu(out.hmenu, 0, kIdFirst, kIdLast, CMF_NORMAL);
    lucidLog(QStringLiteral("  %1 QueryContextMenu -> 0x%2 (%3 items)")
                 .arg(handler.keyName)
                 .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
                 .arg(HRESULT_CODE(hr)));
    if (FAILED(hr)) return false;

    const int count = GetMenuItemCount(out.hmenu);
    for (int i = 0; i < count; ++i) {
        const UINT id = GetMenuItemID(out.hmenu, i);
        if (id == 0 || id == static_cast<UINT>(-1)) continue;  // separator or submenu
        wchar_t text[512] = {};
        if (GetMenuStringW(out.hmenu, static_cast<UINT>(i), text, 511, MF_BYPOSITION) <= 0) continue;
        const QString label = plainMenuText(text);
        lucidLog(QStringLiteral("    item id=%1 text='%2'").arg(id).arg(label));
        if (label.compare(QString::fromWCharArray(kCopyLinkText), Qt::CaseInsensitive) == 0) {
            out.copyLinkOffset = static_cast<int>(id - kIdFirst);
            break;
        }
    }
    return true;
}

// --- clipboard, in raw Win32 -------------------------------------------------
//
// QClipboard belongs to the GUI thread and this runs on a worker, so the
// clipboard is read and written directly. Only CF_UNICODETEXT is handled, which
// is a real limitation and is stated rather than hidden: if the clipboard held
// an image or a file list, a failed invocation cannot put it back.

QString clipboardText() {
    if (!OpenClipboard(nullptr)) return {};
    QString out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = QString::fromWCharArray(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

bool setClipboardText(const QString& text) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        if (text.isEmpty()) {
            ok = true;
        } else if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE,
                                           (static_cast<size_t>(text.size()) + 1) * sizeof(wchar_t))) {
            if (auto* p = static_cast<wchar_t*>(GlobalLock(h))) {
                text.toWCharArray(p);
                p[text.size()] = L'\0';
                GlobalUnlock(h);
                ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
            }
            if (!ok) GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

} // namespace

QList<LucidHandler> discoverLucidHandlers() {
    QList<LucidHandler> found;
    for (const wchar_t* root : kHandlerRoots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &key)
            != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD i = 0;; ++i) {
            wchar_t name[512] = {};
            DWORD cch = 511;
            if (RegEnumKeyExW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) {
                break;
            }
            const QString keyName = QString::fromWCharArray(name);
            // Discovered by the vendor's own registration name rather than by a
            // hard-coded GUID, so a reinstall or a new client generation that
            // registers under a different CLSID is still found. Both generations
            // on the test box register as *Lucid*ContextMenu.
            if (!keyName.contains(QStringLiteral("lucid"), Qt::CaseInsensitive)) continue;

            const QString sub = QString::fromWCharArray(root) + QLatin1Char('\\') + keyName;
            const QString clsid = regString(HKEY_LOCAL_MACHINE, sub, {});
            if (clsid.isEmpty()) continue;

            const QString dll = regString(HKEY_LOCAL_MACHINE,
                                          QStringLiteral("SOFTWARE\\Classes\\CLSID\\%1\\InprocServer32").arg(clsid),
                                          {});
            // Corroboration, not identification: the registration named it, and
            // the server it points at should agree.
            if (!dll.isEmpty() && !dll.contains(QStringLiteral("lucid"), Qt::CaseInsensitive)) continue;
            if (!dll.isEmpty() && !QFileInfo::exists(dll)) continue;

            const bool already = std::any_of(found.begin(), found.end(),
                                             [&](const LucidHandler& h) { return h.clsid == clsid; });
            if (!already) found.push_back(LucidHandler{clsid, keyName, dll});
        }
        RegCloseKey(key);
    }
    return found;
}

QStringList lucidHandlerDescriptions() {
    QStringList out;
    for (const auto& h : discoverLucidHandlers()) {
        out << QStringLiteral("%1 %2 (%3)").arg(h.keyName, h.clsid, h.dllPath);
    }
    return out;
}

bool isSupportedLucidLink(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t.size() > 4096) return false;
    // A link is one token. Anything with whitespace inside it is not one, and
    // this is what stops a stray clipboard change during the wait -- a copied
    // sentence, a path with spaces -- from being accepted as a result.
    if (t.contains(QLatin1Char('\n')) || t.contains(QLatin1Char('\r'))
        || t.contains(QLatin1Char(' '))
        || t.contains(QLatin1Char('\t'))) {
        return false;
    }
    // The two forms the requirement names: the classic scheme, and the newer
    // HTTPS form some installations produce. Trace validates the FORM and never
    // composes one.
    if (t.startsWith(QStringLiteral("lucid://"), Qt::CaseInsensitive)) return true;
    if (t.startsWith(QStringLiteral("https://app.lucidlink.com/"), Qt::CaseInsensitive)) return true;
    return false;
}

LucidSupport probeLucidSupport(const QString& path) {
    LucidSupport out;
    const auto handlers = discoverLucidHandlers();
    lucidLog(QStringLiteral("probe '%1' : %2 handler(s)").arg(path).arg(handlers.size()));
    for (const auto& h : handlers) {
        lucidLog(QStringLiteral("  handler %1 %2 -> %3").arg(h.keyName, h.clsid, h.dllPath));
    }
    if (handlers.isEmpty()) {
        out.reason = QObject::tr("The LucidLink Explorer integration is not installed.");
        return out;
    }
    out.installed = true;

    ShellApartment com;
    lucidLog(QStringLiteral("  apartment=%1 ok=%2")
                 .arg(com.usedOle() ? "OleInitialize" : "CoInitializeEx")
                 .arg(com.ok() ? 1 : 0));
    for (const auto& handler : handlers) {
        HandlerMenu hm;
        if (!buildHandlerMenu(handler, path, hm)) continue;
        if (hm.copyLinkOffset >= 0) {
            lucidLog(QStringLiteral("  SUPPORTED via %1 (offset %2)")
                         .arg(handler.keyName).arg(hm.copyLinkOffset));
            out.supported = true;
            return out;
        }
    }
    lucidLog(QStringLiteral("  no handler offered a copy-link command"));
    // The extension was asked and declined. On a file outside a linked filespace
    // it fails Initialize with E_INVALIDARG and offers nothing, which is the
    // integration's own answer rather than an inference from the path.
    out.reason = QObject::tr("LucidLink does not offer a link for this file. "
                             "Check that the filespace is linked and connected.");
    return out;
}

LucidCopyResult copyLucidLinkViaShell(const QString& path) {
    LucidCopyResult result;
    lucidLog(QStringLiteral("copy '%1'").arg(path));

    const auto handlers = discoverLucidHandlers();
    if (handlers.isEmpty()) {
        result.error = QObject::tr("The LucidLink Explorer integration is not installed.");
        return result;
    }

    ShellApartment com;

    // Snapshot BEFORE anything is invoked. The requirement is that the previous
    // contents survive until the invocation succeeds, and the only way to honour
    // that against an extension that writes the clipboard itself is to be able
    // to put the old value back.
    const QString previous = clipboardText();
    const DWORD beforeSeq = GetClipboardSequenceNumber();

    for (const auto& handler : handlers) {
        HandlerMenu hm;
        if (!buildHandlerMenu(handler, path, hm)) continue;
        if (hm.copyLinkOffset < 0) continue;

        CMINVOKECOMMANDINFOEX info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI;
        info.hwnd = nullptr;
        info.lpVerb = MAKEINTRESOURCEA(hm.copyLinkOffset);
        info.lpVerbW = MAKEINTRESOURCEW(hm.copyLinkOffset);
        info.nShow = SW_SHOWNORMAL;

        const HRESULT hr = hm.menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
        lucidLog(QStringLiteral("  InvokeCommand(offset %1) -> 0x%2")
                     .arg(hm.copyLinkOffset)
                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        if (FAILED(hr)) {
            result.error = QObject::tr("LucidLink refused the copy-link command (0x%1).")
                               .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
            continue;
        }

        // The extension may populate the clipboard asynchronously -- it has to
        // reach its daemon to resolve the file -- so the result is WAITED for
        // rather than read once. The sequence number is the cheap, reliable
        // "did anything change at all" signal; the text is only read when it
        // moved.
        constexpr int kTimeoutMs = 4000;
        constexpr int kPollMs = 20;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < kTimeoutMs) {
            if (GetClipboardSequenceNumber() != beforeSeq) {
                const QString now = clipboardText();
                if (!now.isEmpty() && now != previous) {
                    if (isSupportedLucidLink(now)) {
                        lucidLog(QStringLiteral("  clipboard accepted after %1ms: %2")
                                     .arg(clock.elapsed()).arg(now.trimmed()));
                        result.ok = true;
                        result.link = now.trimmed();
                        return result;
                    }
                    lucidLog(QStringLiteral("  clipboard changed to something unrecognised; restoring"));
                    // Changed into something that is not a link Trace
                    // recognises. Put the old value back rather than leave a
                    // value the user did not ask for and cannot identify.
                    setClipboardText(previous);
                    result.error = QObject::tr("LucidLink returned something that is not a "
                                               "recognised link. The clipboard was restored.");
                    return result;
                }
            }
            QThread::msleep(kPollMs);
        }

        // Nothing arrived. Nothing was overwritten either, so there is nothing
        // to restore -- and saying so is more useful than a bare timeout.
        lucidLog(QStringLiteral("  timed out after %1ms with no clipboard change").arg(kTimeoutMs));
        result.error = QObject::tr("LucidLink did not produce a link within %1 seconds. "
                                   "The clipboard was left unchanged.")
                           .arg(kTimeoutMs / 1000);
        return result;
    }

    if (result.error.isEmpty()) {
        result.error = QObject::tr("LucidLink does not offer a link for this file.");
    }
    return result;
}

} // namespace trace::app

#else // !Q_OS_WIN

namespace trace::app {

QList<LucidHandler> discoverLucidHandlers() { return {}; }
QStringList lucidHandlerDescriptions() { return {}; }
bool isSupportedLucidLink(const QString& text) {
    const QString t = text.trimmed();
    return t.startsWith(QStringLiteral("lucid://"), Qt::CaseInsensitive)
        || t.startsWith(QStringLiteral("https://app.lucidlink.com/"), Qt::CaseInsensitive);
}
LucidSupport probeLucidSupport(const QString&) {
    LucidSupport out;
    out.reason = QObject::tr("The LucidLink integration is Windows-only.");
    return out;
}
LucidCopyResult copyLucidLinkViaShell(const QString&) {
    LucidCopyResult out;
    out.error = QObject::tr("The LucidLink integration is Windows-only.");
    return out;
}

} // namespace trace::app

#endif
