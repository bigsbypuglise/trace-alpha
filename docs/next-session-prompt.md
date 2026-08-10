# Prompt for the next Claude Code session — GATE E, pulled forward

Supersedes the previous version of this file. We are at `627c2ed`, tree clean, nothing
unpushed. Paste everything below the line into a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. **No interface work.** `docs/interface-pass-1-spec-DEFERRED.md` is approved and
   deliberately not started. Do not begin any of it.
3. The goal for this whole phase is the core playback experience alone: smooth playback,
   locked real-time playback, responsive polished scrubbing at slow and fast speeds in both
   directions, and strong GPU integration.

---

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first — §9 (composition rule), §20.5
(source-rate audit), §22 (GATE C) and §23 (cadence characterisation) are the load-bearing
sections. GATE B is passed with owner sign-off; GATE C landed at `e8566a4`.

## The ordering decision, taken

§23.5 recorded the argument and correctly declined to act on it. **The owner's decision is
to pull GATE E — plan §8 item 11, DXGI presentation timing — ahead of items 8, 9 and 10.**

The reasoning, so it is not re-litigated: locked real-time playback is priority #1; §23
measured the residual stutter as **cause A, the integer-tick beat, which is universal and
which only GATE E fixes**; and items 8–10 buy headroom, which §23.4 measured as no longer the
binding constraint on 4444 once the planar path is on. There is no technical dependency from
8, 9 or 10 into 11 — the flip-model swapchain GATE E needs landed at GATE B.

Items 8, 9 and 10 are **deferred, not cancelled**. §22.7 items 2–5 stand.

## 1. Before writing any code — the zero-cost experiment

§23.5 notes that until GATE E *or a default renderer change*, the owner sees the beat **plus**
the cause-B component, because `cpu` is still the default. §23.4 measured that the planar
d3d11 path already removes cause B on 4444: tick jitter **11–14ms → 2–3ms**, worst handler
**55.6ms (over budget) → 37.6ms (under)**, zero handlers over budget.

So: ask the owner to run 4K ProRes 4444 playback twice, once with `TRACE_RENDERER=d3d11` and
once at the default, and say whether the d3d11 run feels better. That costs nothing and buys
two things — it may improve his experience immediately, and it is the only available evidence
for **§23.6, which is still open: why he notices this on 4444 specifically**, when the beat is
identical on 1080p and 422 HQ. Do not assume content or resolution explains it.

If the answer is yes, flipping the default to `d3d11` becomes a live option to take *with*
GATE E rather than after it. Do not flip it unilaterally; it is a product decision.

## 2. GATE E — design before implementation

Write the design into the plan as a new section and get it reviewed before building. The
project has **three reverted scheduler experiments** on record; the difference between them
and this is that this one has a stated composition rule and a measured target.

### The composition rule is already written and is not negotiable

§9: **audio stays the rate and position authority; vsync becomes the phase authority.** Vsync
picks the instant, the audio clock picks the frame for that instant. One owner per question.

This has already been got wrong once in this codebase, and expensively. When audio drove *and*
the wall-clock accumulator also gated presentation, the two clocks beat against each other and
produced matched hold/skip pairs; the fix was to make the audio clock the **only** scheduler.
So when vsync takes the phase question, the accumulator must be **removed from the gating
decision**, not layered underneath it. Adding a third opinion about when to present is how
this becomes revert number four.

### What is actually there today, audited at GATE B (§20.5)

- The exact rational **is stored** (`VideoMetadata::fpsNum`/`fpsDen`, `7b924be`) and
  **nothing reads it** — every consumer goes through `FrameSource::fps()`, a double.
- The tick is `floor(1000.0/fps)`, an **integer-millisecond QTimer** — 41ms at both 24 and
  23.976.
- `Present(0, 0)` is **sync interval 0**: not vsync-throttled, not phase-aligned. DWM
  composites at refresh so at most one present is seen per refresh, but nothing in Trace knows
  the refresh phase.
- The accumulator does not drift — `frameDurationMs` is a double fed by `nsecsElapsed()` and
  carries its residue forward. The tick **bounds** the rate rather than setting it.

GATE E is where the stored rational finally becomes the reference. A rate that is already
rounded cannot be the reference for late-present or jitter.

### Mechanisms to evaluate, and state the choice with reasons

A waitable swapchain (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` plus
`GetFrameLatencyWaitableObject`) and/or `IDXGISwapChain::GetFrameStatistics` for
`PresentCount` / `SyncQPCTime`, against a non-zero sync interval. Say what each gives, what
it costs on the UI thread, and why the chosen one wins — a waitable object that blocks the UI
thread is the same class of mistake as the synchronous remote read that `8b47e08` fixed.

**GATE E only works on the d3d11 backend.** The CPU renderer has no swapchain and therefore no
phase source; it keeps the current tick. That is fine, but it means the cadence fix and the
default-renderer question are coupled — say so in the design rather than discovering it at
sign-off.

### Success criteria — define these before building, not after

The characterisation in §23 is the baseline and gives an unambiguous target:

- **The 62-frame-spaced doubled frames must disappear.** §23 measured median spacing between
  long frames at **61–62 across all six runs**, with `max ≈ 2 × p50` and nothing in the
  1.1–1.5x or >2.5x buckets. That signature going away is the pass condition. A presented-rate
  figure is **not** — it reads 98–99% under the fault and cannot see it, which is the whole
  reason §23 exists.
- The **1080p control** (worst handler 3.8ms against a 41.67ms budget) must show the same
  improvement. It has ten times the headroom and the same beat, so if the fix is real it works
  there first.
- No regression on the **audio-mastered** path: 1080p H.264 99.1%, 4K H.264 98.3%, 4K ProRes
  422 HQ 98.4%, skips 0.
- **Controls must use `TRACE_NO_AUDIO=1`.** 4444 has no audio track while 422 HQ and the 1080p
  clip do, so as shipped they run on different schedulers; comparing them directly would
  "prove" 4444 is uniquely bad when the only difference is which clock is driving.
- **Refresh-rate sweep at ~60 / 120 / 240 Hz** on the Odyssey G95SC, as a regression check.
  Note the prior measurement: at 59 / 119.98 / 240 Hz the counters were flat (99.1 / 99.1 /
  98.7%) and the owner's verdict was "about the same" and "hard to tell". Two runs at a single
  rate span the same range as all three rates. **Do not expect a win there** — the point of
  the sweep is that a display-synchronised path must not be *worse* at any of them, including
  the non-integer 59Hz case where 24fps lands on a 2:3 cadence.
- **Owner sign-off on 4K ProRes 4444 playback.** The harness says the mechanism works; only
  the owner says the stutter is gone. This project has recorded that split six times.

### Scope discipline

Presentation timing only. Do not fold in texture reuse (item 8), GPU scaling (item 9) or
10-bit output (item 10). Do not touch the scrub path — §22.4 established it is unchanged by
GATE C and it must stay that way. Do not change the audio clock; it keeps the rate and
position question.

Commit as `perf(gpu): add DXGI presentation timing` — **GATE E**.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`.
- `V:\` is live client production storage and is strictly read-only.
- Two habits from the last session that are worth keeping: **quote the window size with any
  scrub number** (§22.8 — the HUD carries `win WxH` now), and **check the diff, not the commit
  subjects**, before concluding a range changed nothing.
- Update `CLAUDE.md` and the plan at the end of the session.
