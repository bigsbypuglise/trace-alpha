# Prompt for the next Claude Code session — step 6.5, then GATE C

Paste everything below the line into a fresh session in the repo root.

---

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first — §11 (colorimetry reference),
§17 (GATE B as built) and §17.5 (what is open) are the load-bearing sections for this work.
We are at `fd7a7b6`. Steps 5, 5.5 and 5.6 are signed off; GATE B landed at `8a7cdb3` and is
opt-in behind `TRACE_RENDERER=d3d11`, with `cpu` still the default.

There are two pieces of work, in this order. **Do not start the second before the first is
committed** — GATE C changes what the picture is made of, and building it on a surface
nobody has looked at means a colour or scaling regression will be indistinguishable from a
GATE C bug.

## Part 1 — step 6.5: make GATE B checkable by eye, and cover fullscreen

§17.5 items 2 and 4. Small, and it is the precondition for everything after it.

**1a. Fullscreen through the native surface.** Untested. It is a Qt window state change
with the layout intact (§3), so the child HWND should follow its parent — but that is an
expectation, not a measurement, and a child window is exactly the thing that can be left
behind by a reparent. Test it: enter and leave fullscreen under `TRACE_RENDERER=d3d11` on
both a 16:9 and a non-16:9 clip, on the 5120x1440 panel, and confirm the letterbox viewport
recomputes rather than stretching. Fix whatever it turns up. Also test the surface across a
window move between monitors of different DPI, since that is the same class of bug and is
cheap to check while you are here.

**1b. A CPU-vs-GPU pixel comparison, in the harness.** The honest problem with GATE B is
that every number says "right frame, right size" and none says "right pixels". Build the
check: land the same frame under `TRACE_RENDERER=cpu` and `d3d11`, capture the viewport at
native resolution (the harness already does GetWindowRect + CopyFromScreen — see the
measurement note in `CLAUDE.md`), and report per-channel max and mean delta. This should
be near-zero today, because both paths present the same swscale BGRA buffer; if it is not,
that is a GATE B defect to fix now rather than a GATE C surprise later. Keep it — it
becomes the primary correctness test for GATE C, where the two paths stop being the same
pixels by construction.

**1c. Hand Anj a short visual checklist.** He is the only one who can close §17.5 item 2.
4K ProRes 422 HQ is the bar. Include: the picture at 1:1 and scaled, colour against the CPU
renderer on the *same machine and display* (cross-platform comparisons are not evidence —
see `CLAUDE.md`), letterboxing on a non-16:9 clip, fullscreen, and a re-run by hand of the
two deferred extreme-scrub gestures (4K H.264 fast scrub, ProRes 4444 hard back-and-forth)
to confirm the surface did not make the stall profile worse. Note in the checklist that the
1080p validation clip **opens on a black frame** — the trap that cost a working GATE B
implementation once already — so `Splash_1.mp4` is the file to judge a black picture against.

Commit as `test(render): compare CPU and D3D11 output, and cover fullscreen`.

## Part 2 — step 7, GATE C: planar YUV upload and shader colour conversion

Plan §8 item 7. Only after Anj signs off the GATE B picture.

This is where conversion cost actually moves: today swscale converts on the CPU and the
backend uploads BGRA at 4 bytes per pixel of a frame the GPU could have assembled from 1.5.

### Scope, and what stays out

- **8-bit planar first.** `yuv420p` / `yuv422p` / `yuv444p` as separate R8 textures, matrix
  and range applied in the pixel shader from `ColorInfo`. High-bit-depth ProRes is plan §8
  item 10 and is deliberately a later commit — do not fold it in.
- **Alpha-stripped planar formats must keep working.** `alphaStrippedFormat()` re-describes
  `yuva444p12le` as its alpha-less equivalent by simply never reading plane 3; the planar
  upload path has to preserve that rather than upload a plane the shader ignores.
- **Anything the shader does not handle falls back to swscale BGRA inside the D3D11
  backend** — not to the CPU renderer. Two fallbacks at different levels is how a GPU path
  quietly never engages while the app looks fine, which §12 says to design against. The HUD
  must name which conversion is actually running, the same way `renderer` names the backend.
- `PixelLayout` in `src/core/VideoFrame.h` gains the planar layouts. The header comment
  already anticipates this and says why the enum is Trace's own — **no `AVPixelFormat` in
  that header**, it is reached from the image-sequence path which must compile with
  `TRACE_WITH_FFMPEG` undefined. `FrameBuffer` is documented as "exactly one plane today"
  with accessors shaped so more planes are an addition; hold to that.

### Three traps specific to this step

**The colour must match the CPU path, not merely be plausible.** swscale is already told the
source colorimetry via `sws_setColorspaceDetails` (matrix from the frame, falling back to the
codec context, then to the HD-and-up heuristic). The shader must reproduce the *same* matrix
and range from the same `ColorInfo`, or GATE C books a colour regression that will read as
"the GPU looks different". §11 tabulates the matrices and the normalisation order. Part 1b's
pixel comparison is the test: state a tolerance up front, justify it against 8-bit rounding,
and report the actual deltas per file.

**Do not measure against the wrong baseline.** §9 is explicit: scrub previews already convert
to *display size* in swscale (`b5a56af`, `sws 7.08 → 1.87ms` on 4K ProRes 422 HQ) and draw
1:1. Measuring GATE C against the old full-res-convert-plus-Qt-bilinear path would book a win
that was really "we stopped using a bad filter". The genuine remaining CPU-side target is the
**landing frame** (Step mode), which still converts full-res and lets Qt do a 6.4x downscale.

**Planar upload can make scrub previews slower, and this is the one that will bite.** Today a
4K preview converts to ~display size on the CPU and uploads a small BGRA buffer. Full-res
planar upload sends 1.5 bytes per pixel of the *whole* 4K frame every preview frame — fewer
bytes than full-res BGRA, but far more than the display-size buffer it replaces, and GPU
scaling is plan §8 item 9, i.e. not yet built. Measure the drag path explicitly, per format,
before concluding GATE C is a win. If previews regress, the answer is to keep them on the
display-size CPU path until item 9 lands, not to accept the regression.

### Validation

GATE C is not passed on frame rate. Required, and record it in a `§18.x` validation section
matching the shape of §14.10 / §17.4:

1. Pixel delta CPU vs GPU, per file, per format family, with the tolerance stated first.
2. Full playback pass on all seven test files, both renderers, against the recorded controls
   (playback 98.3%, `rev-hit 97.9%`, `stalls 2 of 394` for the GATE B path).
3. The drag path measured separately for preview cost — see the trap above.
4. `scripts/measure/lifecycle.ps1` in full, including `-PlayThroughDrag` and its
   `-PausedThroughDrag` control. A check that can only report "moving" proves nothing.
5. `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0` on every row, as every prior gate.
6. Owner sign-off on 4K ProRes 422 HQ. The harness says the mechanism works; only Anj says
   the bar holds. This project has recorded that split five times now.

Commit as `feat(gpu): add planar YUV upload and shader colour conversion` — **GATE C**.
`cpu` stays the default until GATE E.

## Working notes

- Build locally with the VS2022/Qt 6.10.2/vcpkg commands in `CLAUDE.md` before pushing; CI
  pins Qt 6.7.2 so a local green is not proof CI is green, but it catches every compile error.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`.
- The `V:\` LucidLink mount is live client production storage and is strictly read-only.
- You cannot push from the sandbox. Commit locally, then have Anj run
  `cd ~/Claude/Trace && git push origin main`.
- Update `CLAUDE.md` and the plan at the end of the session so the next one starts current.
