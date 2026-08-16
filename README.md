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

## Windows distribution (prerelease): portable ZIP only

Trace ships as a **portable Windows ZIP** (no installer yet). It is **beta** from
`v0.2.0-beta.1`.

GitHub Actions is the source of truth for Windows builds.

Workflow behavior:

- **Every push/tag/manual run**: builds on **Windows Server 2022 (VS 2022)** with Qt6 + FFmpeg and uploads:
  - workflow artifact: `trace-windows-x64`
- **Tag pushes matching `v*`**: also create/update a **GitHub prerelease** with:
  - release asset: `trace-windows-x64.zip`

The package name carries **no release stage**, so it does not have to be changed on every
promotion. Releases up to and including `v0.2.0-beta.1` carry the older
`trace-alpha-windows-x64.zip` name and were not renamed retroactively.

Portable ZIP contents include:

- `Trace.exe`
- required Qt runtime files (via `windeployqt`)
- required FFmpeg runtime DLLs

### Download latest Windows build

- **Latest tagged build**: GitHub **Releases** → download `trace-windows-x64.zip`
- **Latest branch/commit validation build**: GitHub **Actions** → latest run → **Artifacts** → `trace-windows-x64`

### Trigger a new build/release

Create and push a tag like:

```bash
git tag v0.2.0-beta.2
git push origin v0.2.0-beta.2
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
