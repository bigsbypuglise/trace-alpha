# The interface pass is the next phase. Priority 2 is LIFTED.

Supersedes the previous version. The bounded reverse-shuttle phase closed with owner
sign-off on 2026-08-10, and the accelerated fast-forward blocker found during that retest
closed with it. **The owner has chosen the next phase: the deferred interface pass.**
Paste everything below the line into a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. ~~**No interface work.**~~ **LIFTED by the owner, 2026-08-10.** The interface pass is
   now the open phase. Priority 1 is unchanged and is now the binding constraint *on this
   work*: every phase of it must be measured against the playback and scrub baselines, and
   a feature that costs smoothness loses.
3. **Smooth, responsive motion beats matching final-frame fidelity during motion.**
   Fidelity is owed to the frame the user stops on, not to the frames flying past on the way
   there. This settles a whole class of trades in advance — preview resolution, preview
   filtering, sampling stride, paint pacing, shuttle sampling — so **do not re-open any of
   them on picture-quality grounds alone.** Four instances now: the drag preview, §15's
   scrub sampling, accelerated reverse, and accelerated forward.

## What has been accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10, owner retest)** — fast-forward advancing clearly through
the complete 2×/5×/10×/30× ladder on every format; reverse 30× reading as intentional at the
approved ~15fps presentation cadence; direction changes responding correctly; stopping
landing on the last visibly displayed frame; and no regression to normal playback, audio
return, scrubbing, exact release or stepping.

**Read each at its stated width.** What was accepted in the shuttle phase is the **engine**.
A later summary that says "the transport is done" has widened it, and the widened version is
wrong — see the deferred list.

## Deferred, with the conditions attached

- **The 2×/5×/10×/30× interface.** Approved, specified, unstarted. It carries one
  requirement that is easy to lose: **the buttons must begin at 2× on their first click**,
  while the J/L keyboard convention keeps 1× as its first rung. The owner confirmed both
  readings on 2026-08-10. `startShuttleRun(direction, stride)` takes any stride, so this is
  a call site rather than engine work. Full spec in
  `docs/interface-pass-1-spec.md`; the note is inline at *Fast-forward behavior*.
- **Step 10, 10-bit display output** — two external gates, both outside the code: a
  10-bit-capable display confirmed, and the intended Windows Advanced Color / HDR /
  colour-management workflow defined. §9's warning still holds — this is 10-bit **output**,
  not the high-bit-depth **processing** that shipped at GATE C.
- **Mixed-monitor DPI (§20.4)** — tabled for hardware. `AllScreens` returns one display and
  Parsec replaces it rather than adding one, so this is **not executable on this box at
  all**. Do not re-propose it until a second monitor exists.
- **BT.2020 has no tonemap** (§22.7 item 5). Known gap, never a complaint. §28.4 applies if
  picked up: the shader averages before the matrix because both remaining steps are affine.
- **GATE E step 2 — vsync snapping** — stopped by owner decision, design retained unbuilt at
  §24.4–24.6. `DwmGetCompositionTimingInfo` fails on this machine, so any future E2 is
  d3d11-only via `IDXGISwapChain::GetFrameStatistics`. **Do not start it without a specific
  new cadence complaint.**
- **LucidLink read-ahead.** Two designs measured worse; try full-request buffered serving
  before partial reads, then benchmark. Not in progress.
- **EXR / image sequences and OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI, so EXR
  does not open today. Largest untouched area, and a feature rather than a fix.

## THE OPEN PHASE — interface pass 1

The owner chose it on 2026-08-10 and lifted priority 2 to allow it. The spec is
`docs/interface-pass-1-spec.md` — approved, complete, and written by the owner.
**Rename it** (drop `-DEFERRED`) and replace its "do not begin any of this" header with the
lift, as the first commit, so the tree stops saying two different things.

### Do this before planning anything: re-derive §2 of that spec

§2 is a list of ten dependencies and conflicts, and it was written on **2026-08-09** —
before the reverse shuttle existed, before step 9, before `d3d11` became the default, and
before the fast-forward shuttle. **A deferred item's premise expires** is the rule this
project has re-learned five times, and it applies to a document as much as to a note.

At least one item is already known stale: **§2 item 2 says reverse shuttle "will likely
defer on first pass" because continuous reverse is GOP-walk bound.** It is not. The engine
shipped at `e9fd236`/`dd21fe9`, was measured, and has owner sign-off — 4K H.264 reverse 1×
went 87.0 → 99.2% of real time, and the full 2×/5×/10×/30× ladder works in both directions.
So the spec's capability-detection-and-defer branch is no longer the expected outcome; the
control is a **call site** onto `startShuttleRun(direction, stride)`.

Check the other nine the same way rather than reading them as still true.

### Then take the two GPU prerequisites first, because they are the structural blockers

Both were deliberately left unbuilt because only this pass needs them. Neither is UI work.

1. **The composited overlay, built for real.** §2 item 1 is the largest structural item in
   the whole spec. Qt child widgets over the D3D11 child HWND are neither visible nor
   hit-testable (§19/§20.1), so the auto-hiding floating transport cannot exist without it.
   Renderer-composited translucency was *proven* to work with real alpha, full native input,
   keyboard staying with Qt via `MA_NOACTIVATE`, and **no measured playback cost** — but
   `TRACE_OVERLAY_COMPOSITED` is explicitly a disposable spike with placeholder art. Promote
   it to a real path, and **re-measure the playback cost** rather than citing the spike's
   number; the spike held a static overlay visible through one 9s run, which is not the same
   as an animated fade during a scrub.
2. **A view-transform contract on `VideoRenderer`.** §2 item 6. Rotate/flip has nowhere to
   live today. The D3D11 backend already computes a letterbox viewport, which is the natural
   home; the CPU backend applies it in its `QPainter`. Add it to the interface so both
   backends implement one contract — separately hacking the two paths is what the spec's own
   fallback exists to prevent.

### Then the spec's own phasing, from its phase 1

Its 14 phases are already ordered and each is meant to be an independently reviewable
commit. Phase 1 is a read-only audit — **report the boundaries before implementing**, which
is the same discipline that made the reverse-shuttle phase work.

### Priority 1 is the constraint on all of it

Every phase runs the playback and scrub regression, not just the last one. `cadence.ps1`,
`scrub.ps1`, `lifecycle.ps1` and `stalls_vs_window.ps1` exist and the baselines are recorded.
Quote `hitch` and `win WxH`. The overlay and the auto-hide animation are the two items most
likely to cost something, and they are early — measure them when they land, not at phase 14.

## Loose ends worth knowing about, none of them blocking

- **30× is only honestly measurable on the 412-frame 1080p clip.** Every other test file
  traverses in under 0.2s at that speed, so those cells read "clip-limited". A longer clip
  would make that row trustworthy on all four formats.
- **`outside` — per-present time that is not the handler — is 3.7–15ms and unattributed**
  (`docs/reverse-shuttle-plan.md` §10 item 3). It no longer binds anything: the shuttle holds
  presentation at 24/s, or 15/s snapped, at every speed. It would matter again to any future
  work that wants a higher presentation rate.
- **`TRACE_SCRUB_FILL_MS` ships at 60, not the 240 that §15.2's decision records.** A/B'd on
  4K H.264 backward drag and it changes nothing measurable, so this is a
  documentation-versus-code discrepancy rather than a regression to chase. Correct the note
  rather than the default, and re-measure before doing either.
- **Long-GOP slice-only threading is a closed question** — measured, refuted, and the knob
  (`TRACE_LONGGOP_SLICE_THREADS=1`) is retained as the control. Frame threading is what makes
  the GOP walk cheap.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Six instances now —
§26.2, §27, §28, §29.1, the BGRA cache-pricing term that had been wrong since GATE C, and
§15.3's decline of directional prefetch, which was correct for the drag and the opposite of
correct for a fixed-rate reverse run.

**Check what a number is measured against before believing it.** GATE E's `jitter` read 34ms
on a schedule within 1.8ms of its deadline; `stalls` read 51 on a run with 3 real hitches;
`total` under-reported by 3.47ms/frame; §9's "local contrast within 0.7%" concluded there was
no scaling defect when there was a large one; §29.1's "~2.3x" converted decoder throughput
into a drag speed.

**A statistic over a quantity is not the quantity.** The keyframe grid was first learned as
`max(walk) + 1`, which converges *from below* and stopped at 41 on a 48-frame grid — so every
"snapped" target missed and still walked, while the HUD read `SNAP gop 41` and nothing
improved. The keyframe *positions* were exact and available all along.

**A validated path is not a validated feature.** §29.2 is the sharpest instance: GATE E was
validated on the Play action alone and every other path that started the timer kept compiling
silently. Enumerate the entry points rather than testing the one the harness drives —
`scripts/measure/revtransitions.ps1` exists for exactly this and covers all six shuttle exits.

**Reproduce on the reported case AND on a healthy one before theorising.** The fast-forward
fault was reported as affecting every format. It did — but 4K H.264 still reached 3.97× of 4×
while ProRes 4444 delivered 1.00× when asked for 2×. Measuring both is what showed it was one
shared fault with a per-format threshold rather than four bugs.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file",
not "a drag is in progress". Guarding the shuttle on it disabled the entire pipeline while
every counter looked healthy, with `posted 0` on the worker line as the only symptom.

## `d3d11` is the default renderer, and two obligations follow

`TRACE_RENDERER=cpu` is the control and the escape hatch — **the first thing to try if
anything about the picture looks wrong** — but it is now the *softer* picture as well as the
slower one, since step 9 only fixed the GPU path. Say so when telling anyone to try it.

**Every scrub and playback baseline taken before 2026-08-10 was on `cpu`** and most are not
tagged with a renderer. They remain valid as records; they are **not** valid as comparisons
against a run taken today. Re-tag as you re-measure.

## Parsec — ask which display a session is on before comparing any number

Mid-session display mode changes are Anj logging in over Parsec. Remote sessions present a
virtual display at **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @ 239.999Hz**.
`scripts/measure/refresh.ps1` reports the current one.

Window size dominates cache depth and stall counts (§22.8), and resolution moves with the
refresh rate, so a Parsec run differs in two ways at once and neither shows in a bare stall
figure. And **no subjective smoothness, cadence or picture-quality judgement is valid over
Parsec** — it captures, re-encodes and re-times the screen. Every owner sign-off in this
project has been taken at the machine, including the shuttle one.

## Quote `hitch`, not `stalls`, and quote `win WxH` with either

`stalls` counts paint gaps over `2 × refresh` — 8.3ms at 239.999Hz, 33.3ms at 60Hz — so the
same run reads `stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. **`hitch` is a fixed 33ms bar
and is the only stall figure comparable across sessions.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token, so CI runs and logs can be
  read off the API — `printf 'protocol=https\nhost=github.com\n\n' | git credential fill`
  then curl with `Authorization: Bearer`. Note `git credential fill` needs the `host` field
  or it refuses.
- **CI asserts the renderer initializes**: the workflow runs `Trace.exe
  --renderer-selftest=d3d11` and fails on a fallback (exit 3), on the backend not being built
  (exit 4), or on `planar=0`. It does not `show()` and does not draw a frame, so the step-9
  reduction loop has never executed on WARP (§28.6 item 7).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop
  a running `Trace.exe` first** or the link fails with LNK1104.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles
  every `§` into mojibake. Use `cat` from the Bash tool. **And a `git commit -m` here-string
  containing `>` or `->` fails with `unknown switch`** — write the message to a file and use
  `git commit -F`. **Do not name a PowerShell helper `Diff`**: `diff` is a built-in alias for
  `Compare-Object` and aliases outrank functions, so the helper is never called.
- Harness: `revplay.ps1` (both directions — `-Forward` drives L), `revtransitions.ps1` (all
  six shuttle exits), `scrub.ps1` (`-SnapRelease` for anything about the landing;
  `-Reversals` does not guarantee one), `lifecycle.ps1` (**run both `-PlayThroughDrag` and
  `-PausedThroughDrag`**), `cadence.ps1`, `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `sidebyside.ps1`, `stalls_vs_window.ps1`, `abfilter.ps1`/`croprect.ps1`/`previewshot.ps1`.
  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`), and **check nothing overlapped it** — a
  foreground window spoiled one capture in this session.
- Update `CLAUDE.md` and the plans at the end of the session.
