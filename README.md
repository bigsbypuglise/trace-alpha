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

## Build

Trace uses CMake.

```bash
cmake -S . -B build
cmake --build build
```

> Note: FFmpeg support depends on your local environment and CMake configuration.

## Validation

Use the focused Windows media validation checklist:

- [`docs/windows-validation-checklist.md`](docs/windows-validation-checklist.md)

## Windows portable build (GitHub Actions)

This repo includes a Windows release workflow that builds and packages:

- `trace-alpha-windows-x64.zip` (portable)

Tag pushes matching `v*` create a prerelease with the zip attached.

## Known limitations

- Build/test matrix is still being established
- Public release packaging not finalized
- Validation is in progress for H.264 MP4 and ProRes MOV edge cases
