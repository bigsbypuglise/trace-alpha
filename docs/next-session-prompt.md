# After GATE E

Supersedes the previous version of this file. GATE E is passed with owner sign-off and
the playback stutter is gone. Paste everything below the line into a fresh session in the
repo root.

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

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first — §9, §22 (GATE C), §23 (the
cadence characterisation), §24 (GATE E) and §25 (the default-renderer flip) are the
load-bearing sections.

## What just happened, and the one thing not to undo

**GATE E passed at step 1** (`e2b8655`). The playback tick was a fixed integer-millisecond
`QTimer` at `floor(1000/fps)` — 41ms against a 41.667ms frame — so presents landed on a
41ms grid and every interval was 41 or 82ms, never 41.667. It is now re-armed per frame
against an absolute deadline built from the source's exact rational. Doubled frames went
from 5 per 11s to 0, and all three audio-mastered files improved.

**The owner signed off running the CPU default**, having just double-clicked the app. That
is the fact to carry: E1 alone cleared the complaint on the renderer that still has a
cause-B component. Do not re-litigate §23.6 (why 4444 specifically) — the fault is gone
and the evidence with it.

**GATE E step 2 — vsync snapping and the present/decode swap — is deliberately NOT built.**
The design is retained unbuilt at plan §24.4–24.6. Its case largely evaporated under
measurement: on the planar path the residual is about one refresh with nothing in the
doubling bucket, and E2 costs a frame of latency plus a reshaped tick handler. Two premises
also changed: `DwmGetCompositionTimingInfo` **fails on this machine**, so there is no
renderer-independent phase source and any future E2 is d3d11-only via
`IDXGISwapChain::GetFrameStatistics`; and the panel is **239.999 Hz**, exactly 10 refreshes
per 24.000fps frame. **Do not start E2 without a specific new cadence complaint.**

`TRACE_DEADLINE_SCHED=0` restores the old scheduler in the same binary. It is the negative
control for any cadence measurement and it still shows the fault.

## `d3d11` is now the default renderer (2026-08-10)

The owner tested both side by side and chose it. Plan §25 has the measured case and
the verification; the short version is that on 4K ProRes 4444 it takes doubled frames
1 -> 0, handlers over budget 1 -> 0, worst present gap 62.5 -> 45.9ms and conversion
16.6 -> 5.6ms. **`TRACE_RENDERER=cpu` is now the control and the escape hatch, and it
is the first thing to try if anything about the picture looks wrong.**

**Two obligations follow and they are easy to forget.**

**Every scrub and playback baseline in the plan was taken on `cpu`**, and most are not
tagged with a renderer because there was only one default. They remain valid as records;
they are *not* valid as comparisons against a run taken today. Re-tag as you re-measure,
and quote `win WxH` with any stall or scrub number (§22.8 — stall counts are a function
of window size and dominate).

**The untested-DPI gaps are now the shipping path.** Real mixed-monitor DPI has never
run (§20.4), the box has one display, and its mode was observed changing mid-session on
2026-08-10 — 5120x1440 @ 239.999Hz in the morning, 1920x1200 @ 60Hz in the afternoon.
Never assume a recorded refresh rate or geometry still holds; `scripts/measure/refresh.ps1`
reports the current one, and it matters because 24fps is exactly 10 refreshes at 240Hz
and a 2:3 cadence at 60Hz.

## Candidate next work, in rough order

Nothing here is started. Pick with the owner rather than assuming.

1. **The remaining scrub complaint: stalls.** This is the oldest live owner-facing issue
   and it is where the scrub complaints have always lived. See the "Known open items" list
   in `CLAUDE.md` — item 1. 4K H.264 carries 30–116ms gaps on cache misses; ProRes 422 HQ
   measures `stalls 0 of 438`, so the bar is reachable. **Read §22.8 first**: stall counts
   are a function of window size and dominate, so any number quoted without `win WxH` is
   not checkable, and `scripts/measure/stalls_vs_window.ps1` is the sweep.
2. **4K ProRes 4444 fast drag** — still decode-bound at ~15.4ms/frame, ~2.3x playback
   against the owner's stated ~4x. Not a bug; an explicit product decision about whether to
   skip frames on the heaviest media or run the worker ahead of the request chain.
3. **Deferred GPU items 8, 9, 10** — texture/upload reuse, GPU scaling, 10-bit output.
   These buy headroom, and §23.4 established headroom is no longer the binding constraint
   on playback. Item 9's honest target is the Step landing (§9).
4. **LucidLink read-ahead** — two designs measured worse; try full-request buffered serving
   before partial reads, then benchmark. Not in progress.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`.
- `V:\` is live client production storage and is strictly read-only.
- **Quote the window size with any scrub or stall number** (§22.8 — the HUD carries
  `win WxH`), and **check the diff, not the commit subjects**, before concluding a range
  changed nothing.
- **A derived metric whose inputs changed meaning reads as a catastrophic result, not as a
  broken metric.** GATE E's `jitter` field read 34ms on a schedule that was within 1.8ms of
  its deadline, because the timer is re-armed after the handler and the reference had
  quietly become decode cost. Check what a metric is measured *against* before believing a
  number that moved by an order of magnitude.
- Harness: `scripts/measure/sidebyside.ps1` (both backends on screen at once, with a
  readback that proves which one each window actually adopted), `cadence.ps1` (cadence distribution — the only thing that can
  see a beat; presented rate cannot), `playhud.ps1` (taller crop, for `rep`/`skip` and the
  audio line), `refresh.ps1` (the display's true rational rate), `lifecycle.ps1`,
  `scrub.ps1`, `stalls_vs_window.ps1`. **Cadence controls need `TRACE_NO_AUDIO=1`** — 4444
  has no audio track while 422 HQ and the 1080p clips do, so as shipped they run on
  different schedulers.
- Update `CLAUDE.md` and the plan at the end of the session.
