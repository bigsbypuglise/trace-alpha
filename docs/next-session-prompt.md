# After the scrub-stall pass

Supersedes the previous version of this file. GATE E is passed with owner sign-off, the
playback stutter is gone, `d3d11` is the default renderer, and the scrub-stall item is
largely closed. Paste everything below the line into a fresh session in the repo root.

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
**That cause is now known: it is Anj logging in over Parsec.** Remote sessions present a
virtual display at 1920x1200 @ 60Hz; the physical panel is 5120x1440 @ 239.999Hz. So ask
which one a session is on before comparing to a record — resolution moves with the refresh
rate, so a Parsec run also has a different window geometry, and window size dominates cache
depth and stall counts (§22.8); the two effects arrive together and neither shows in a bare
stall figure. **And no subjective smoothness, cadence or picture-quality judgement is valid
over Parsec** — it captures, re-encodes and re-times the screen, imposing its own pacing and
its own lossy compression. Owner sign-offs on feel must be taken at the machine.
Never assume a recorded refresh rate or geometry still holds; `scripts/measure/refresh.ps1`
reports the current one, and it matters because 24fps is exactly 10 refreshes at 240Hz
and a 2:3 cadence at 60Hz.

## The scrub-stall pass, 2026-08-10 — read this before touching scrub

Plan §26 has it in full. Three things carry.

**`stalls` was measured against the display and this box changes mode.** It counts paint
gaps over `2 × refresh` — 8.3ms at 239.999Hz, 33.3ms at 60Hz. The same 4K H.264 run reads
`stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. **Quote `hitch`; it is the only stall figure
comparable across sessions.** This is also most of §21.4's unexplained "2 of 394 vs 44 of
375" — §22.8 was right that window size matters and wrong that the rest was machine state.

**§15.5 item 1 is closed as answered-no.** Converting Step and cache-fill conversions to
display size would now *cost* — GATE C already made full-res 1080p entries 3.11MB planar
plane sets rather than 8.29MB BGRA, and the remaining 18% would be bought by replacing a
0.25ms plane copy with a swscale resample. **A deferred item's premise expires. Re-derive
before building.**

**What the misses needed was bytes.** The reverse-cache budget is 384MB now (was 192):
1080p `hitch 8 → 2`, 4K H.264 `hitch 3 → 1` with worst gap 169.6 → 80ms. `TRACE_REVERSE_CACHE_MB`
is the control and the fallback.

**The footprint is APPROVED** (owner, 2026-08-10): working set 396 → 598MB at 1080p and
677 → 902MB at 4K is acceptable for a professional 4K review application, and performance
stays the priority. §26.5 is the owner-requested verification that followed — the cache is
bounded (six consecutive scrub runs plateau; 382.2 of 384MB after 1357 inserts and 1245
evictions), discarded on a file change (working set 920 → 254MB), and playback-neutral
(identical rate, frames, doubling bucket and `handler>budget` across three files at both
budgets). **Do not add adaptive caching or pool changes off the back of this** — that was
explicitly declined.

## Candidate next work, in rough order

Nothing here is started. Pick with the owner rather than assuming.

1. ~~**The owner's subjective scrub test on the finished build**~~ — **DONE, PASSED
   2026-08-10** (plan §26.6). The picture feels good on the 384MB cache with `d3d11`
   default. §26 is closed and nothing about scrub stalls carries forward as open.
2. **4K ProRes 4444 fast drag** — still decode-bound at ~15.4ms/frame, ~2.3x playback
   against the owner's stated ~4x. Not a bug; an explicit product decision about whether to
   skip frames on the heaviest media or run the worker ahead of the request chain. Note
   4444 gains least from cache work and that is structural (§26.3), so this is untouched.
3. **The convert pool is sized in pre-GATE-C currency** (§26.4 item 2). It prices the
   smallest entry as BGRA, so at 1080p it provisions ~50 buffers for a cache holding 129.
   `alloc` is 0.61–0.65ms of a 32ms frame at 4K — visible, not binding. Left alone on
   purpose so it would not confound the budget measurement. **The owner declined it as
   part of the cache work**, so it needs raising with him rather than picking up.
4. ~~**Deferred GPU items 8, 9, 10**~~ — **8 is CLOSED answered-no and 9 is DONE**
   (2026-08-10, plan §27/§28). Only **step 10, 10-bit output**, is still deferred, and it
   needs an `R10G10B10A2` swapchain and a display in 10-bit mode — a different thing from
   the high-bit-depth *processing* that shipped at GATE C. Read the §28.6 open items before
   picking anything up here; two of them are owner decisions.
5. **LucidLink read-ahead** — two designs measured worse; try full-request buffered serving
   before partial reads, then benchmark. Not in progress.

## The scaling pass, 2026-08-10 — read before touching the picture

Plan §27 and §28 in full. Three things carry.

**Step 8's premise had expired and step 8 is closed.** GATE B's own lazy creation already
reused everything — `tex 3` across 261 frames of 4444 playback, `tex 4` across a 406-paint
reversal drag — and the residual upload is memcpy bandwidth (56.6MB in 3.47ms = 16.3 GB/s).
A staging buffer is strictly more work and its justification needs the draw to be the
constraint, which at `draw 0.01ms` it is not. **Second deferred item in two sessions whose
premise expired; §26.2 was the first. Re-derive before building.**

**Every path in Trace was undersampling the downscale, and now the default one is not.**
Measured against ffmpeg references at the exact drawn size: d3d11 **0.74**, cpu **0.73**,
swscale drag preview **0.76** on an axis where `area` is 0 and `neighbor` is 1. A box
reduction in the shader takes 4444 to **0.02** and 422 HQ from 0.89 to **0.00**, with no
measurable playback or scrub cost. `TRACE_GPU_REDUCE=0` is the control and is exact.
**This is why §9's "local contrast within 0.7%" saw nothing, and it re-reads §20.3/§21.2 —
CPU and D3D11 agreeing was never evidence either was right.**

**Two owner decisions came out of it.** The drag preview is still 0.76, so the picture now
*sharpens* on release where it used to match; the fix is one swscale flag but previews are
the drag path where supply is 19% on 4444, so **do not flip it without measuring the shuttle
rate**. And `TRACE_RENDERER=cpu` is now the softer picture as well as the slower one, which
matters when telling anyone to try it. **Owner visual sign-off on the new picture is
outstanding and must be taken at the machine, not over Parsec.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token, so CI runs and logs can
  be read straight off the API — that is how the renderer self-test below was verified.
- **CI asserts the renderer initializes now** (`b5ad4d2`, `a36f10d`): the workflow runs
  `Trace.exe --renderer-selftest=d3d11` against the deployed folder and fails on a
  fallback (exit 3), on the backend not being built (exit 4), or on `planar=0`. First run
  read `renderer=d3d11 fellback=0 planar=1`. If you add a renderer capability that can
  degrade silently, add it to that line.
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
  `scrub.ps1`, `stalls_vs_window.ps1`, and for scaling quality `abfilter.ps1` (calibrated
  area-to-point axis, with `-Sensitivity` that refuses material too smooth to resolve
  anything), `croprect.ps1` (video rect out of a window capture, size asserted against the
  HUD) and `previewshot.ps1` (captures with the button held, so it is a preview and not a
  landing). **Never use Trace as its own reference for a filtering question.**
  **Cadence controls need `TRACE_NO_AUDIO=1`** — 4444
  has no audio track while 422 HQ and the 1080p clips do, so as shipped they run on
  different schedulers.
- Update `CLAUDE.md` and the plan at the end of the session.
