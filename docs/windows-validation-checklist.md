# Trace Windows Validation Checklist (Practical)

## Test matrix

Run all tests on:

- H.264 MP4 (CFR)
- ProRes MOV

## Recommended media characteristics

Use at least:

1. H.264 MP4, CFR 24 fps, 1080p, 20–60s, long-GOP
2. H.264 MP4, CFR 30 fps, 1080p, 20–60s
3. H.264 MP4 from variable/timing-uneven source (phone/screen capture style)
4. ProRes 422 MOV, 24 fps, 1080p, 20–60s
5. ProRes 422 HQ MOV, 30 fps, 1080p or 4K, 20–60s
6. Very short edge clips (2–5s) in both MP4 and MOV

Prefer clips with frame counter burn-in or obvious visual frame landmarks.

---

## Manual test cases

### 1) Baseline playback (MP4)
- Open MP4
- Play 5–10s, pause, resume
- Repeat 3x at start/middle/end

### 2) Baseline playback (ProRes MOV)
- Open MOV
- Same play/pause cycle as MP4

### 3) Single-frame stepping
- Pause on a known frame
- Step Right x10
- Step Left x10
- Repeat near clip start and near clip end

### 4) Determinism/repeatability
- Pick frame N
- Run sequence `Right x5`, `Left x5` three times
- Confirm identical ending frame/image each run

### 5) Short seek/jump correctness
- From paused state run sequences like:
  - `Right x3`, `Left x2`, `Right x7`
- Confirm final frame matches expected net movement

### 6) Long movement via key-repeat
- Hold Right to move far forward
- Hold Left to move back
- Verify frame/image alignment on release

### 7) Playback stability run
- Continuous play for 2–3 minutes
- Pause and immediately single-step
- Repeat for MP4 and MOV

### 8) HUD readout correctness
- Toggle readout modes (Frame / Seconds / Timecode)
- Validate at frame 0, 1, fps, and 10*fps

---

## Pass/fail criteria

Pass when all are true:

- 50 consecutive single-step operations with no mismatch
- Net frame movement always lands on expected frame
- No visible cumulative drift in 2–3 minute playback
- HUD frame/seconds/timecode mutually consistent
- ProRes behavior is comparable to H.264 (no codec-specific regressions)

Fail if any issue is reproducible 2+ times.

---

## Failure symptoms to watch for

- Off-by-one frame after step/seek
- HUD frame changes but image does not (sticky frame)
- Single tap advances two frames
- Pause after long play lands on unexpected frame
- ProRes-specific jitter/wrong frame after jump
- Timecode boundary errors at exact second rollover

---

## Next implementation milestone (based on results)

If any of the above fails, prioritize:

**Internal decode-position verifier + mismatch logging** (no new UI)

This should compare expected logical frame index vs decoded PTS-derived frame index during step/seek/play to isolate mapping vs seek vs decode-path issues quickly.
