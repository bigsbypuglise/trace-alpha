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

## Windows portable build (GitHub Actions)

GitHub Actions is the source of truth for Windows builds.

Workflow behavior:

- **Every push/tag/manual run**: builds on **Windows Server 2022 (VS 2022)** with Qt6 + FFmpeg and uploads a workflow artifact:
  - `trace-alpha-windows-x64.zip`
- **Tag pushes matching `v*`**: also create/update a **GitHub prerelease** and attach:
  - `trace-alpha-windows-x64.zip`

### Download latest Windows build

- **Tagged alpha builds**: GitHub **Releases** page (prerelease asset)
- **Branch/commit validation builds**: workflow run artifacts in the **Actions** tab

### Trigger a new alpha build/release

Create and push a tag like:

```bash
git tag v0.1.0-alpha.1
git push origin v0.1.0-alpha.1
```

That tag triggers the Windows build and publishes/updates the prerelease asset.

## Known limitations

- Build/test matrix is still being established
- Public release packaging not finalized
- Validation is in progress for H.264 MP4 and ProRes MOV edge cases
