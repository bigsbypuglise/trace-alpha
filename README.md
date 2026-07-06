# Trace (alpha)

Trace is a lightweight desktop media viewer/playback tool focused on frame-accurate review workflows.

## Current scope

- Open stills, image sequences, and video files
- HUD transport/readouts (frame, seconds, timecode)
- Keyboard-first playback and frame stepping
- FFmpeg-backed video decode path (when enabled)

## Status

This repository is currently **alpha / experimental**.

Recent work focused on:

- improving FFmpeg seek behavior
- deterministic frame-step behavior
- stabilizing playback timing logic

## Build (local, optional)

Trace uses CMake.

```bash
cmake -S . -B build
cmake --build build --config Release --target Trace
```

> Note: FFmpeg support is optional and depends on your local environment/toolchain.

## Validation

Use the focused Windows media validation checklist:

- [`docs/windows-validation-checklist.md`](docs/windows-validation-checklist.md)

## Windows distribution (alpha): portable ZIP only

For the current alpha stage, Trace ships as a **portable Windows ZIP** (no installer yet).

GitHub Actions is the source of truth for Windows builds.

Workflow behavior:

- **Every push/tag/manual run**: builds on **Windows Server 2022 (VS 2022)** with Qt6 + FFmpeg and uploads:
  - workflow artifact: `trace-alpha-windows-x64.zip`
- **Tag pushes matching `v*`**: also create/update a **GitHub prerelease** with:
  - release asset: `trace-alpha-windows-x64.zip`

Portable ZIP contents include:

- `Trace.exe`
- required Qt runtime files (via `windeployqt`)
- required FFmpeg runtime DLLs

### Download latest Windows build

- **Latest tagged alpha build**: GitHub **Releases** → download `trace-alpha-windows-x64.zip`
- **Latest branch/commit validation build**: GitHub **Actions** → latest run → **Artifacts** → `trace-alpha-windows-x64`

### Trigger a new alpha build/release

Create and push a tag like:

```bash
git tag v0.1.0-alpha.1
git push origin v0.1.0-alpha.1
```

That tag triggers the Windows build and publishes/updates the prerelease ZIP asset.

Release-note wording reference: [`docs/release-notes-alpha.md`](docs/release-notes-alpha.md)

## Future installer note (not in current scope)

Consider adding a Windows installer later when all are true:

- packaging layout is stable
- playback behavior is stable enough for broader testing
- file associations / Start Menu shortcuts become important

## Known limitations

- Build/test matrix is still being established
- Validation is in progress for H.264 MP4 and ProRes MOV edge cases
