# The playback phase is ACCEPTED. Next is reverse shuttle, as a bounded phase.

Supersedes the previous version of this file. The core-playback phase is formally
accepted by the owner (2026-08-10). Candidate item 1 is struck. A GATE E regression on
the J-K-L path was found and fixed. **The next focused engineering item is continuous
reverse playback / reverse shuttle**, and it is to begin with **measurement and an
architecture proposal, before any implementation.** Paste everything below the line into
a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. **No interface work.** `docs/interface-pass-1-spec-DEFERRED.md` is approved and
   deliberately not started. Do not begin any of it.
3. ~~The goal for this phase is the core playback experience alone.~~ **ACCEPTED AS
   COMPLETE, 2026-08-10** — see below. Superseded by priority 5.
4. **Smooth, responsive scrubbing takes priority over matching final-frame scaling quality
   during motion** (2026-08-10). Fidelity is owed to the frame the user stops on, not to the
   frames flying past on the way there. This settles a whole class of trades in advance —
   preview resolution, preview filtering, sampling stride, paint pacing — so **do not re-open
   any of them on picture-quality grounds alone.** **This priority now extends to the reverse
   shuttle**, where the owner has stated it explicitly as "at accelerated reverse speeds, do
   not require every source frame."
5. **Reverse shuttle is the current phase**, and it is bounded. See the brief below.

## The phase that just closed — what was accepted, and what was NOT

**Accepted as complete (owner, 2026-08-10):**

- smooth **forward** playback;
- exact real-time scheduling;
- responsive **bidirectional scrubbing**;
- the **SDR** D3D11 GPU integration.

Note each of those four is narrower than the shorthand people will reach for. It is
*forward* playback, not playback in general — continuous reverse is explicitly the next
item and was not accepted. It is the *SDR* GPU integration — 10-bit output and HDR are
out. Do not let a later summary widen any of them.

**Formally deferred, with conditions:**

- **Step 10, 10-bit display output.** *"Not a playback-performance or GPU-integration
  blocker for the current SDR base version. Do not build it until we confirm a
  10-bit-capable output display and define the intended Windows Advanced Color / HDR /
  colour-management workflow."* Two gates, both external to the code: hardware confirmed,
  and a workflow defined. §9's warning still holds — do not conflate it with the
  high-bit-depth *processing* that shipped at GATE C.
- **Mixed-monitor DPI (§20.4).** Tabled for want of hardware. `AllScreens` returns one
  display and Parsec replaces it rather than adding one, so this is **not executable on
  this box at all** — it is not merely untested. Do not re-propose it as work until a
  second monitor exists. What *is* reachable single-display: a runtime scale change on the
  primary (a real `WM_DPICHANGED`, swapchain resize, video-rect recompute) plus a code
  audit of that path.
- **BT.2020 has no tonemap** (§22.7 item 5). Known gap, never a complaint. If picked up,
  §28.4 applies: the shader averages before the matrix because both remaining steps are
  affine, and a tonemap between them breaks that.
- **LucidLink read-ahead.** Two designs measured worse; try full-request buffered serving
  before partial reads, then benchmark. Not in progress.
- **EXR / image sequences and OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI, so
  EXR does not open today. Largest untouched area, and a feature rather than a fix.

---

# THE CURRENT PHASE — reverse shuttle

## Why now, in the owner's reasoning

The planned interface includes **2x, 5x, 10x and 30x rewind**. Reverse at 1x currently
measures **86.7% of real time** on 4K H.264. Exposing rewind controls on top of that would
surface a known weakness, so the engine work comes first. This is a case of the interface
spec driving an engine requirement — it is not interface work, and starting it is not a
breach of priority 2.

## Product goals, as stated by the owner

Treat reverse shuttle as a **separate bounded phase**. At accelerated reverse speeds,
**do not require every source frame.**

- immediate response when rewind is pressed;
- stable, intentional visual cadence;
- newest-target-wins behaviour;
- no UI-thread saturation;
- rapid direction changes;
- appropriate frame sampling at 2x, 5x, 10x and 30x;
- **exact frame landing when rewind stops**;
- **no regression** to forward playback, scrubbing, stepping, or audio state.

Read that list as two halves. The first six are about *motion* and are where sampling is
licensed. The last two are the invariants, and they are where every previous reverse or
scrub attempt in this project actually failed — `e76eabb` (a stale frame under a new
name), the reverted async attempts (frame order), and the July 2026 scrub exception that
displayed keyframe 30 for seven consecutive frames while the HUD asserted `delta 0`.

## The instruction on how to start

**Begin with measurement and an architecture proposal, before implementation.** Reuse the
validated asynchronous scrub/cache infrastructure where appropriate, but **do not weaken
exact scrub release or increase normal playback cost.**

So the first deliverable is a measurement pass and a design, not a commit that moves
pixels. The project has twice reverted async decode attempts that were built before the
ordering contract was settled (`a171e3a`/`1d280eb`, reverted `9cd2a0c`/`a2f7999`); §6 is
the post-mortem and is worth reading before proposing anything.

## What is already known, and should not be re-derived

- **Reverse 1x on 4K H.264 measures 86.7% of real time**, `handler>budget 11 of 110
  (max 111.1ms)`, `seeks 13`, `rev-hit 88.5%`, cadence `p50 41.8 / p95 123.6 / p99 150.1ms`
  (plan §29.3). `win 1280x829`, d3d11, physical panel. **This is the first honest reverse
  measurement in the project** — everything recorded between GATE E and 2026-08-10 was
  measuring the J-K-L scheduler fault instead (§29.2), so treat older reverse figures as
  void rather than as history.
- **The cost is the GOP walk**, not decode and not conversion. A backward step that misses
  the reverse cache pays a seek plus a walk to the target.
- **ProRes is structurally different and must be measured separately.** Every frame is a
  keyframe, so a backward seek lands on the target, no intermediate frames are produced and
  **there is nothing to cache** — `rev-hit` reads 0.0% on 4444 and that is not a cache
  failure. Do not tune a single mechanism against both codec families.
- **§15's sampling gate is the precedent for the sampling half**, and its four *failed*
  gate inferences are recorded in `computeScrubStride`'s comment — a latch, a decaying
  mean, a mean per request, and a mean per seek with a threshold. All four look reasonable
  and all four measured wrong. The gate that worked is `AV_CODEC_PROP_INTRA_ONLY`, asked of
  the codec. **A reverse shuttle at 30x on long-GOP is exactly the case that gate exists to
  refuse**, so sampling there needs a different mechanism, not the same one turned up.
- **The reverse cache is 384MB** (`TRACE_REVERSE_CACHE_MB`), verified bounded and discarded
  on file change (§26.5). 768MB was measured and is past the knee.
- **The seek-walk cache fill budget is 240ms** on the worker (§15.2), and it exists because
  a walk fills the cache on its way through — one miss becomes a run of hits.
- **Directional prefetch is declined** (§15.3, §26.4 item 3) on the grounds that the worker
  has no idle time on the files that hitch. **That reasoning was about the drag path.**
  Continuous reverse at a fixed rate is a different workload — the target is predictable,
  which is exactly what a drag's is not — so the decline does not automatically carry over.
  Re-derive it rather than citing it, and re-derive it rather than assuming the opposite too.
- **Audio is 1x forward only** and every off-speed and reverse path is deliberately silent.
  The guard is in the tick. "No regression to audio state" means a reverse run must still
  leave `userPlayIntent_` and the audio path exactly as it found them.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Four instances in
three sessions now — §26.2, §27, §28, and §29.1, which was stale **two days** after it was
written. The pattern is not carelessness: a note records a *conclusion* while the thing
that expires is the *measurement underneath it*. §29.1 read as an open product question
when the decision it asked for had already shipped and been signed off.

**Check what a number is measured against before believing it.** Fifth instance now.
GATE E's `jitter` read 34ms on a schedule within 1.8ms of its deadline (§24.13). `stalls`
read 51 on a run with 3 real hitches (§26.1). `total` under-reported by 3.47ms/frame
because it never included the upload (§27.4). §9's "local contrast within 0.7%" concluded
there was no scaling defect when there was a large one (§28.1). And §29.1's "~2.3x"
converted decoder throughput into a drag speed, which stopped being a valid conversion the
day sampling shipped.

**A validated path is not a validated feature.** §29.2 is the newest instance and the
sharpest: GATE E moved playback from a free-running timer to a timeline that has to be
*established*, was validated on the Play action alone, and every other path that started
the timer kept compiling silently. **Reverse shuttle will add new entry points into the
playback machinery. Enumerate them and test each one**, rather than testing the one the
harness happens to drive.

## Things not to undo

- **GATE E step 2 — vsync snapping and the present/decode swap — is deliberately NOT built**
  and is stopped by owner decision. Design retained unbuilt at §24.4–24.6.
  `DwmGetCompositionTimingInfo` **fails on this machine**, so any future E2 is d3d11-only
  via `IDXGISwapChain::GetFrameStatistics`. The panel is **239.999Hz**, exactly 10 refreshes
  per 24.000fps frame. **Do not start E2 without a specific new cadence complaint.**
- **The drag preview's remaining softness is ACCEPTED AS-IS and CLOSED, not deferred**
  (§28.6 item 2). Only an *observation* reopens it — the change on release becoming visibly
  objectionable in normal use — never a number.
- **The convert pool is still sized in pre-GATE-C currency** (§26.4 item 2) and **the owner
  declined changing it**. `alloc` is 0.61–0.65ms of a 32ms frame at 4K — visible, not
  binding. Raise it with him rather than picking it up.
- **Upscaling is deliberately unfiltered** (§28.6 item 5). The guard is `fitted < content`
  on both axes.
- **Adaptive caching and convert-pool changes were explicitly declined** off the back of the
  384MB cache work.
- **`beginPlaybackTimeline()` must be called by any new path that starts `playTimer_`**
  (§29.3). That is the whole content of the §29.2 fix. If reverse shuttle adds a way to
  start playback, it goes through that function or it reproduces the 792ms fault.

## `d3d11` is the default renderer, and two obligations follow

`TRACE_RENDERER=cpu` is the control and the escape hatch — **the first thing to try if
anything about the picture looks wrong** — but it is now the *softer* picture as well as
the slower one, since step 9 only fixed the GPU path. Say so when telling anyone to try it.

**Every scrub and playback baseline in the plan taken before 2026-08-10 was on `cpu`** and
most are not tagged with a renderer. They remain valid as records; they are **not** valid
as comparisons against a run taken today. Re-tag as you re-measure.

## Parsec — ask which display a session is on before comparing any number

Mid-session display mode changes are **Anj logging in over Parsec**. Remote sessions
present a virtual display at **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @
239.999Hz**. `scripts/measure/refresh.ps1` reports the current one; this session's runs
were all on the physical panel.

Window size dominates cache depth and stall counts (§22.8), and resolution moves with the
refresh rate, so a Parsec run differs in two ways at once and neither shows in a bare stall
figure. And **no subjective smoothness, cadence or picture-quality judgement is valid over
Parsec** — it captures, re-encodes and re-times the screen. Owner sign-offs on feel must be
taken at the machine. **This matters more for reverse shuttle than for anything so far**:
"stable, intentional visual cadence" is a feel judgement by definition.

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
- **CI asserts the renderer initializes** (`b5ad4d2`, `a36f10d`): the workflow runs
  `Trace.exe --renderer-selftest=d3d11` and fails on a fallback (exit 3), on the backend not
  being built (exit 4), or on `planar=0`. **It does not `show()` and does not draw a frame**,
  so the step-9 reduction loop has never executed on WARP (§28.6 item 7).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop
  a running `Trace.exe` first** or the link fails with LNK1104.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles
  every `§` into mojibake. Use `cat` from the Bash tool. **And a `git commit -m` here-string
  containing `>` or `->` fails with `unknown switch`** — write the message to a file and use
  `git commit -F`.
- Harness: `scrub.ps1` (`-SnapRelease` for anything about the landing; `-Reversals` does not
  guarantee one), `lifecycle.ps1` (**run both `-PlayThroughDrag` and `-PausedThroughDrag`** —
  a check that can only report "moving" proves nothing), `cadence.ps1` (the only thing that
  can see a beat; presented rate cannot), `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `sidebyside.ps1`, `stalls_vs_window.ps1`, `abfilter.ps1`/`croprect.ps1`/`previewshot.ps1`
  for scaling quality. **Cadence controls need `TRACE_NO_AUDIO=1`** — 4444 has no audio track
  while 422 HQ and the 1080p clips do, so as shipped they run on different schedulers.
- **There is no reverse-shuttle harness yet.** `revplay.ps1` was written ad-hoc this session
  (click near the end of the groove, then press J) and was not committed. Building a proper
  one is part of the measurement deliverable — it needs to drive J repeatedly for the
  2x/5x/10x/30x speeds, capture the landing frame on stop, and assert exactness.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`).
- Update `CLAUDE.md` and the plan at the end of the session.
