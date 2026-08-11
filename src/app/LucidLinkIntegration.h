#pragma once

#include <QString>
#include <QStringList>

namespace trace::app {

// The installed LucidLink Windows integration, driven through shell interfaces.
//
// WHY THIS AND NOT THE DAEMON'S REST API. LucidLink runs a local REST service
// (the CLI's own `--rest-endpoint` option) and it is authoritative: for the
// nominated file, `GET /fsEntry?path=...` returns `"id" : "2955:105901"`, which
// is exactly the identifier in the expected link. But **no endpoint returns a
// link** -- it returns the ingredients, and assembling
// `lucid://<filespace>/file/<id>/<name>?reveal=true` from them means hard-coding
// LucidLink's URL format, which the requirement forbids in as many words. The
// vendor's own shell extension does exactly that assembly internally (its
// binary carries the literals `lucid://`, `/file/` and `?reveal=true`), which is
// the point: that format belongs to LucidLink, and newer installations may
// return an `app.lucidlink.com` HTTPS link instead. Asking the extension to
// produce the link is what makes Trace accept whatever the installed
// integration actually emits.
//
// The REST API is still useful and is NOT used here only because it cannot
// answer this question; it remains the right tool for validating an id.
//
// EVERYTHING IN THIS FILE MUST RUN OFF THE UI THREAD. It creates COM objects,
// loads a third-party DLL and waits on a clipboard. The spec forbids blocking
// shell calls on the UI thread and priority 1 outranks this whole pass.

// A handler that looks like the LucidLink integration, found in the registry
// rather than hard-coded. Both generations were present on the test box --
// `LucidContextMenu` (Program Files\Lucid) and `LucidLinkContextMenu` (Common
// Files\LucidLink) -- so the CLSID is discovered and every candidate is tried.
struct LucidHandler {
    QString clsid;      // "{b5fd958e-...}"
    QString keyName;    // the ContextMenuHandlers subkey it was registered under
    QString dllPath;    // InprocServer32, used only to corroborate the vendor
};

// Registered context-menu handlers whose registration names them as LucidLink's.
// Empty means the integration is not installed on this machine.
QStringList lucidHandlerDescriptions();
QList<LucidHandler> discoverLucidHandlers();

// What the integration says about ONE file. Read-only: this builds the
// extension's menu and reads it, and invokes nothing.
//
// The discrimination is the extension's own. On a file outside a linked
// filespace `IShellExtInit::Initialize` returns E_INVALIDARG and no command is
// offered; on a LucidLink file it returns S_OK and offers its commands. That is
// the integration declaring support for the file, which is the authoritative
// answer the storage classifier cannot give -- the classifier only ever
// establishes the necessary condition that the volume is a virtual mount.
struct LucidSupport {
    bool installed = false;   // at least one handler is registered
    bool supported = false;   // a handler offered the copy-link command for this file
    QString reason;           // why not, when not
};
LucidSupport probeLucidSupport(const QString& path);

// Runs the copy-link command and returns what the integration put on the
// clipboard. Never composes a link itself.
struct LucidCopyResult {
    bool ok = false;
    QString link;
    QString error;
};
LucidCopyResult copyLucidLinkViaShell(const QString& path);

// Whether `text` is a link form Trace is willing to hand back. Exposed because
// it is a rule about LucidLink's output rather than about this call path, and
// because a validator that is never tested against a bad string is not a
// validator.
bool isSupportedLucidLink(const QString& text);

} // namespace trace::app
