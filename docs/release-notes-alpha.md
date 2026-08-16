# Trace prerelease notes policy

Trace is currently **beta** (from `v0.2.0-beta.1`, 2026-08-15) and distributed for Windows as a
**portable ZIP only**. It was alpha through `v0.2.0-alpha.1`; the filename of this document is
kept as it is so links to it do not break.

Current supported package:

- `trace-windows-x64.zip`
  - `Trace.exe`
  - required Qt runtime files
  - required FFmpeg runtime DLLs

**The package name carries no release stage, from 2026-08-15.** It read
`trace-alpha-windows-x64.zip` through `v0.2.0-beta.1`, so a tester downloading the beta got a
file named alpha. Renaming it was deliberately kept out of the release commit — a typo in any
one reference breaks publication rather than a build, and **only two of the nine references run
on a tag at all**, so an ordinary green build could not have spoken for them.

It is now defined **once**, as `DIST_NAME` at the top of
`.github/workflows/windows-release.yml`, and every use site references it. A stage word there
would have to be changed in lockstep with the three `(beta)` literals in
`src/app/MainWindow.cpp` on every promotion; a name that never has to change cannot fall out of
step with them.

**Releases up to and including `v0.2.0-beta.1` keep the old asset name** — published assets were
not renamed retroactively, so a link to one still works.

Distribution sources:

- **GitHub Releases** (tag builds `v*`) for prerelease assets, published with `prerelease: true`
- **GitHub Actions artifacts** (push/manual runs) for validation builds

Not in current scope:

- NSIS / Inno Setup / MSI installer
- file-association installer logic
- auto-updater system

Revisit installer work later when packaging and playback behavior are stable and Start Menu/file-association UX becomes important.

## How a release's notes are published

`docs/release-body.md` holds the hand-written notes for the **current** tag and is published as
the GitHub release body (`body_path` in `.github/workflows/windows-release.yml`);
`generate_release_notes: true` appends the commit log after it. Rewrite that file as part of
cutting a release.

**It must name the known gaps plainly.** A prerelease with honest limits gets useful bug
reports; one without gets reports about things already known. As of `v0.2.0-beta.1` those are:
8K ProRes 4444 XQ does not reach real time and is understood rather than unsolved (13.64 fps
full-quality, decode alone is 94% of the frame budget), EXR does not open, HDR/BT.2020 has no
tonemap, 10-bit *output* is unsupported (distinct from the high-bit-depth *processing* that does
work), multi-monitor DPI is validated at 100%/150% only, and cold LucidLink delivery is
~600-800 Mbps so multi-Gbps plates will not stream.

**A gap that has been measured must be stated as measured, with the number.** `v0.2.0-alpha.1`
listed mixed-monitor DPI as unvalidated, which was true and was the named reason that release
was not a beta; closing it on hardware is what promoted this one. The 8K entry replaces it as
the release's headline limitation and is a different kind of statement — not "untested" but
"tested, quantified, and not reachable on this hardware with this decoder". Do not let a later
rewrite soften it into a vague slowness note.

## What the app must say about itself

**The version comes from `project(Trace VERSION ...)` in the top-level `CMakeLists.txt`**,
through `TRACE_VERSION_STRING` into About and the Report an Issue mail body. Bump it with the
tag, or every issue report carries the wrong build.

**The stage — alpha, beta — is NOT in that number.** CMake's `VERSION` field cannot hold a
prerelease suffix, so `0.2.0` covers both the alpha and the beta of this line, and the word is a
literal in three places in `src/app/MainWindow.cpp`: `buildIdentity()`'s `Trace %1 (beta)`, the
About dialog's small print, and the Report an Issue mail subject. Changing stage means editing
all three. Missing one leaves the number looking right while the build names the wrong stage in
the one place a tester will quote it back.
