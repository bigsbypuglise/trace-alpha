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
