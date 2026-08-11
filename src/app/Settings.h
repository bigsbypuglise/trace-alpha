#pragma once

#include <QString>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace trace::app {

// THE ONE SETTINGS HOME FOR THE WHOLE APPLICATION.
//
// Spec phase 11 introduces Trace's first persistent state (the recent-file
// list) and therefore has to answer a question nothing before it did: where
// does persistent state live. Owner decision, 2026-08-11 -- portable INI first,
// then a per-user INI. The reasoning, because inheriting the default here would
// have been wrong in a way nobody would notice for months:
//
//   QSettings' Windows default is NativeFormat, which writes
//   HKCU\Software\<org>\<app>. Trace ships as a PORTABLE ZIP with no installer
//   by deliberate choice (docs/release-notes-alpha.md), and an app that leaves
//   registry keys behind after its folder is deleted contradicts that outright.
//
// So:
//
//   1. A `trace.ini` sitting beside Trace.exe, IF IT EXISTS AND IS WRITABLE.
//      Its presence is how a user asks for portable mode -- creating an empty
//      file is the whole gesture. Trace never creates it, because creating it
//      would make every installation portable and the choice would stop being
//      one.
//   2. Otherwise `trace.ini` under QStandardPaths::AppConfigLocation, which is
//      per-user, always writable, and survives an upgrade.
//
// The writability test in step 1 is what makes "always writable" a property
// rather than a hope: a copy unzipped into Program Files can carry a trace.ini
// it cannot write to, and silently discarding every setting is worse than
// falling back.
//
// ESTABLISHED ONCE, WITH A SINGLE OWNER, BECAUSE THREE OTHER THINGS ALREADY
// WANT IT: phase 6's fullscreen geometry (in memory only today), phase 13's
// window state, and spec section 4's `Lock Window to Media Aspect Ratio`, which
// is specified as checked by default and therefore needs somewhere to remember
// having been unchecked. Same single-gate pattern as `hasSourceTimecode_` and
// `OverlayModel::enabledByEnvironment()`: one answer, asked by everyone.
QSettings& settings();

// Writes through to disk. Callers do this after a change rather than relying on
// QSettings' destructor, because the object below is deliberately leaked -- see
// the .cpp -- and because a setting that survives a clean quit but not a crash
// is a setting that will be reported as random.
void syncSettings();

// "portable" or "appconfig". For the diagnostics HUD: which home is in force is
// not answerable by looking at the window, and phase 8 established that a UI
// state that cannot be read from a capture needs a HUD field rather than a
// squint at a screenshot.
const char* settingsModeName();

// The file actually in use. Absolute, native separators.
QString settingsFilePath();

} // namespace trace::app
