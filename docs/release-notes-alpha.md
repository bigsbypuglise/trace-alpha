# Trace alpha release notes policy

Trace is currently **alpha** and distributed for Windows as a **portable ZIP only**.

Current supported package:

- `trace-alpha-windows-x64.zip`
  - `Trace.exe`
  - required Qt runtime files
  - required FFmpeg runtime DLLs

Distribution sources:

- **GitHub Releases** (tag builds `v*`) for alpha prerelease assets
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

**It must name the known gaps plainly.** An alpha with honest limits gets useful bug reports;
one without gets reports about things already known. As of `v0.2.0-alpha.1` those are: EXR does
not open, HDR/BT.2020 has no tonemap, 10-bit *output* is unsupported (distinct from the
high-bit-depth *processing* that does work), mixed-monitor DPI is unvalidated, and cold
LucidLink delivery is ~600-800 Mbps so multi-Gbps plates will not stream.

**The version the app reports comes from `project(Trace VERSION ...)` in the top-level
`CMakeLists.txt`**, through `TRACE_VERSION_STRING` into About and the Report an Issue mail body.
Bump it with the tag, or every issue report carries the wrong build.
