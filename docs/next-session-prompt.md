# OPEN: 8K ProRes 4444 XQ playback — owner REJECTED the frame-drop result (2026-08-13)

**The 8K file is NOT solved and must not be described as solved.** The real-time drop holds
media time and the owner has rejected the result: **~11 visible fps reads as visibly poor
playback.** The same file plays perfectly in QuickTime on a *less powerful* macOS machine.
`TRACE_RT_DROP` stays as an experimental fallback and control; **frame dropping is not the
final solution.** The acceptance target is ~100% media time at 24fps source cadence with
`drop 0`, full quality, no recurring hitches, and exact pause / scrub release / stepping.

## THE DECISION GATE IS ANSWERED: DECODE-ONLY EXCEEDS 24 fps, SO THE BOTTLENECK IS INSIDE TRACE

Decode-only, no conversion, no upload, no render, no seek, one persistent decoder per
configuration over the whole file (`scripts/measure/decbench/`, plus the same sweep on the
GCC-built reference). 7680x4320, ProRes **4444 XQ** (profile 5), `yuva444p12le`, **12-bit,
4:4:4 with alpha** (4 planes, log2chroma 0/0), **24000/1001**, 5,739 Mbps, 145 frames.

| build | threads | fps | % of real time |
|---|---|---|---|
| vcpkg / MSVC (**what Trace links**) | auto (0) | 16.19 | 67.5 |
| vcpkg / MSVC | 16 | 16.12 | 67.2 |
| vcpkg / MSVC | **32** | **21.29** | **88.8** |
| GCC (gyan 9.0) | auto (0) | 20.16 | 84.1 |
| GCC (gyan 9.0) | 24 | 25.59 | 106.7 |
| **GCC (gyan 9.0)** | **32** | **29.00** | **121.0** |

**Two independent multipliers, and neither is a Trace algorithm.** `thread_count = 0` is not
"all of them" — FFmpeg's automatic count caps at 16 and warns above it, leaving half a
32-thread box idle. And the **MSVC build is 27% slower than the GCC build at the same thread
count** (21.29 vs 29.00). Together they are **1.79x**.

**THE FIRST 8K CEILING FIGURE IN THIS REPO WAS WRONG AND THIS IS WHY.** "20.5 fps = 85%, the
machine cannot do it" was measured with the winget `ffmpeg.exe` — a **GCC** build — while Trace
links an **MSVC** one, and at the default thread count. It substituted a different program for
the one being asked about. **Benchmark the libraries you ship, at the settings you ship.**

## WHERE THE TIME GOES IN TRACE, AND IT IS STRICT SERIALIZATION

Sequential 1x playback, `TRACE_RT_DROP=0`, physical panel, `win 1226x1083 / display 1226x690`:

| | dec | sws | upload+handoff | handler | outside | presented |
|---|---|---|---|---|---|---|
| default threads | 61.11 | 17.07 | 12.25 | **90.11** | 2.49 | 44.7% |
| `TRACE_DECODE_THREADS=32` | 45.08 | 17.21 | 12.03 | **74.55** | 1.84 | 54.2% |

`handler` is the sum of its parts to within 0.3ms and **`outside` is under 2.5ms**, so nothing
is waiting: decode, conversion and upload run strictly one after another on the UI thread.
Latency distribution at t=32: p50 75.4 / p95 82.2 / p99 83.2 / max 118.5ms against a 41.71ms
budget.

**Item 7 is answered and clean: ordinary 1x playback does NOT seek, flush or rebuild.**
`walk 0f`, `seek n=1` (the open), `io play | seq 100.0% | seek 0 | stall 0`, `recov 0`,
`ctx-rebuilds 0`, `drop 0`. The pipeline is doing the right work; it is doing it in series.

## THE PROPOSED FIX, AND WHY IT NEEDS BOTH HALVES

Neither half reaches 24fps alone, and the arithmetic says so before any code is written:

- **Async sequential decode alone**: decode moves to a worker and overlaps conversion+upload, so
  the rate becomes `max(decode, convert+upload)` = `max(45.1, 29.2)` = 45.1ms = **22.2 fps.
  Still short.**
- **A faster FFmpeg build alone**: decode 45.1 -> ~33ms, but still in series with 29.2ms of
  conversion and upload = 62ms = **16 fps. Worse than the other half.**
- **Both**: `max(33, 29.2)` = 33ms = **~30 fps**, comfortably clear of the 41.71ms budget with
  `drop 0`. **That is the target and it has margin.**

So the order is: **(1) establish a faster FFmpeg build** — the MSVC/GCC gap is the single
largest unexplained factor and needs confirming as a build property rather than a measurement
artefact; **(2) prototype the bounded async sequential decode queue** the owner conditionally
authorised, now that UI-thread serialization is measured rather than assumed. Its contract:
sequential only, bounded queue, **no routine seeking, no stale-frame delivery**, and it must not
touch the scrub lease, the exact release landing, stepping or reverse.

**`TRACE_DECODE_THREADS=32` is worth +21% in Trace today** (dec 61.11 -> 45.08ms, 44.7 -> 54.2%)
and is one line, but it is left at the default because it is not the fix and a default should be
set once, from the build that ships.

## What is NOT established

- **VLC was attempted and produced no usable number** — `-I dummy --benchmark` returns
  immediately on this box. Item 8 is open. The vcpkg-vs-GCC pair on one machine already
  separates *build* from *Trace pipeline*, which is the substantive half of what item 8 was for.
- **The QuickTime comparison has not been explained and must not be hand-waved.** Apple Silicon
  carries a hardware ProRes decoder and that is the obvious hypothesis, but it is a hypothesis;
  it is also irrelevant to the gate, which is already answered against Trace.
- **Nothing here has been signed off.** No image quality was reduced, no proxy, no resolution
  change.

---

## The record of the 2026-08-12 report and the first investigation

## The report

Playback is **very slow** on two files, and **another player on the same machine is not slow on
them**:

1. `C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\13_4448x3096_ProRes_4444`
2. `C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\12_8K_ProRes4444`

Neither is in the validated asset set — every figure in this repo was taken at 4096×2304 or
below. **Both are ProRes 4444 and both are far larger than anything measured.** That is the
shape of the problem: several decisions in this codebase were taken on measurements at 4K and
their premises may not survive a 4.5× or 7.5× increase in pixels.

## ASK THIS BEFORE ANYTHING ELSE — the two players may not have the same contract

**Trace never drops frames by design.** "Video playback never skips frames — heavy files slow
down rather than drop frames. Deliberate: ordering over rate." A player that looks smooth on an
8K 4444 plate may simply be dropping frames to hold real time, which is the normal thing for a
review player to do and is a *different product decision*, not a faster decoder.

So establish, before treating this as a defect: **which player**, and **is it presenting every
frame?** Play a clip with visible motion or burnt-in frame numbers for a fixed wall-clock
duration in both and compare the frame each lands on. If the other player is at frame 240 after
10 seconds and Trace is at frame 90, Trace is genuinely slower. If both are at 240 but the other
one visibly stutters or reports dropped frames, the comparison is contract versus contract and
the answer is a product decision the owner has to take, not a bug.

Report which of those two it is before proposing any fix.

## Measure the breakdown first — the HUD already splits every term

One playback run per file, `TRACE_NO_AUDIO=1`, on the physical panel, quoting `win WxH` and
`display`. The terms that decide the whole investigation are already instrumented: `dec`,
`sws`, the per-frame handler against the frame budget, `outside`, `presented … % real time`,
and the `io`/`src` lines. Do not theorise before that table exists — and take it on **4K 4444
as the control**, since that file is measured, healthy, and differs only in size.

## Three candidate causes, in the order they are cheap to test

**(1) INTRA-ONLY CODECS ARE FORCED TO SLICE-ONLY THREADING, AND THAT DECISION'S PREMISE HAS
PROBABLY EXPIRED.** `VideoDecoderFFmpeg.cpp:988` — `intraOnly ? FF_THREAD_SLICE : (FF_THREAD_FRAME
| FF_THREAD_SLICE)`. The comment above it gives the reason: frame threading pipelines across
frames, so every seek+flush stalled ~thread-count packets, *"scrubbing lag ~100ms per request on
4K ProRes"*.

**That cost was measured in July 2026, on the UI thread, before the async scrub worker existed.**
`f77d472` (August) moved random-access scrub decode onto a worker under a lease, so the stall
that justified the trade is no longer on the thread that matters. What the trade *costs* is
forward playback throughput on intra-only codecs — and that cost scales with frame size, which is
exactly why it is invisible at 1080p and 4K and would dominate at 4448×3096 and 8K. It is also
exactly why another player, which frame-threads, would not be slow.

**There is no knob for this direction.** `TRACE_LONGGOP_SLICE_THREADS=1` tests the opposite
(slice-only on long-GOP, measured and refuted). **Add the symmetric one** — e.g.
`TRACE_INTRA_FRAME_THREADS=1` giving intra-only codecs `FF_THREAD_FRAME | FF_THREAD_SLICE` — and
A/B it. If it is the cause, the fix is not simply flipping the default: **re-measure scrub, step
and reverse on 4444 with it on**, because that is what the original decision was protecting, and
`revplay.ps1` / `scrub.ps1 -SnapRelease` / the transitions matrix are the checks. A resolution- or
`intraOnly`-conditional default is a legitimate outcome; so is "frame threading everywhere now
that scrub is off the UI thread". Measure, then choose.

**(2) THE PLANAR UPLOAD IS MORE BYTES THAN BGRA ON 4444, AND STEP 8 WAS DISMISSED AT 4K.**
GATE C's own record says *"Planar is not always fewer bytes: 4:4:4 12-bit is 56.6MB of planes
against 37.7MB of BGRA"* — at 4096×2304. At 8K that ratio holds and the absolute numbers do not:
three 12-bit planes at 8192×4320 is **~212MB per frame**, against ~141MB of BGRA. At 24fps that is
**~5 GB/s of upload alone**, before decode. Step 8 (texture/upload reuse) was closed as
*answered-no* on the grounds that upload was memcpy bandwidth at 16.3 GB/s and texture churn was
already minimal — **measured at 4K**. A third of memory bandwidth is a different conclusion from a
twentieth.

`TRACE_PLANAR_UPLOAD=0` is the control and costs nothing to run: it restores swscale BGRA on the
d3d11 path. If BGRA is *faster* on these two files, that inverts GATE C's conclusion at high
resolution and the fix is a size-conditional choice, not a revert. Also worth one run of
`TRACE_RENDERER=cpu` to see whether the GPU path is implicated at all.

**(3) IT MAY BE I/O, IN WHICH CASE NO DECODE WORK HELPS.** 8K ProRes 4444 is an enormous bitrate —
the storage work measured a 9K 4444 plate at **4497 Mbps** with 11.5MB per video packet, and cold
LucidLink delivering ~610 Mbps. Check the `io` and `src` lines: `src local` versus a mount, read
latency, and whether the reads are keeping up. If the file is on a slow local disk or a share,
that is the answer and it is a different fix entirely.

## Rules that still bind

Priority 1 is unchanged and this *is* priority 1 work. Whatever is proposed must not weaken
exact scrub release, exact stepping, frame ordering, or the never-skip-a-frame invariant — the
one sanctioned exception remains the active drag preview on intra-only media (§15), and
accelerated shuttle speeds. **A fix that makes 8K play smoothly by dropping frames is a product
decision for the owner, not an implementation choice.**

Re-measure the standing regression on the validated asset set before and after any change.
`hitch`, `win WxH` and `display` on every figure; scratch `TRACE_SETTINGS_FILE` on cadence runs;
`-Env TRACE_TRANSPORT_BAR=1` for every harness that scans for the docked groove.

---

## What shipped, and at what width

**`v0.2.0-alpha.1`** (`ce6e46b`, tagged 2026-08-11). Fifteen phases of interface work: the
floating transport, the bidirectional shuttle interface, fullscreen consolidation and auto-hide,
Time Display with real source timecode, the Share menu and LucidLink links, view transforms,
Open Recent, media-shaped windows, the Movie Inspector, the full menu structure with Help and
the accessibility proxy tree, and view scaling with pan.

**The minor bump was deliberate and so was staying alpha.** `v0.1.0-alpha.23` was the previous
tag and continuing that sequence would have understated an entire new interface surface. It is
**not** beta because **real mixed-monitor DPI has never executed** — §4's window-shaping system
takes `dpr` as an argument and has only ever been driven synthetically. Trace's audience runs
multi-monitor at mixed scaling, so that is the gap a wider audience hits first.
**`v0.2.0-beta.1` once a second display validates it**, and that is the obvious next step.

**The version the app reports lives in `project(Trace VERSION ...)`** in the top-level
`CMakeLists.txt` and flows through `TRACE_VERSION_STRING` into About and the Report an Issue
mail body. **Bump it with the tag** — it was 0.1.0 while the tag said 0.2.0 until this session
caught it, which would have put the wrong build in every issue report.

---

## The likely next items, in no particular order — the owner chooses

1. **Mixed-monitor DPI (§20.4), once the second display exists.** This is the named beta gate
   and the only thing standing between here and `v0.2.0-beta.1`. What has never run: a real
   `WM_DPICHANGED`, a monitor-to-monitor move, a swapchain resize across it, fullscreen on a
   secondary display. `Trace.exe --window-shape-selftest` covers the arithmetic across 11 shapes
   x 4 scale factors and **prints its own caveat on its last line** precisely so the limit
   travels with the result. **Synthetic DPR is not mixed-monitor validation and must never be
   quoted as such.**
2. **EXR and image-sequence review, with OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI,
   so EXR does not open at all today. This is the largest missing *format* capability and it is
   roadmap item 7.
3. **HDR / BT.2020 tonemap.** The correct matrix is applied and there is no tonemap, so PQ/HLG
   material looks wrong on both backends. Known gap, never a complaint — but it is now named in
   public release notes, so a report may arrive. **Note §28's ordering constraint: the box
   average happens before range normalisation and the matrix because both are affine; that
   equivalence does NOT hold with a tonemap in between.**
4. **10-bit display output (step 10).** Formally deferred with **two external gates, both
   outside the code**: a confirmed 10-bit-capable display, and a defined Windows Advanced Color
   / HDR workflow. Do not conflate it with the high-bit-depth *processing* that shipped at
   GATE C.
5. **LucidLink read-ahead.** Two designs measured worse. The next experiment is stated at the
   read-ahead section of `CLAUDE.md`: satisfy FFmpeg's read callback only when the *complete*
   requested byte count is buffered, so read sizes stay ~5MB and the demuxer never repositions.
   **Benchmark before committing**, with the injected-latency knob, because a real cold cache is
   a one-shot.

**Carried as polish, not work:** the pan cursor, wanted only if it can be made trivial **and
identical on both backends** — it cannot today, because the D3D11 surface owns its own
window-class cursor and answers `WM_SETCURSOR` itself while the CPU path inherits the widget's.
**Do not describe it as a one-liner.**

**Carried as a design-package detail, not a spec item:** the temporary rate chip is top-left; the
approved package's §6 puts it centred above the transport. It was moved to top-left at phase 8
because at 84px of panel height a top-right chip overlaps a 34px control.

**Known to be OUT and staying out:** Check for Updates (no updater exists), GATE E step 2 /
vsync snapping (**stopped by owner decision — do not start it without a specific new cadence
complaint**), and §23.6 (why 4444 specifically stuttered — the fault is gone and the evidence
with it; do not re-open it speculatively).

---

## What phase 16 established that outlives it

**A NEGATIVE GREP IS ONLY EVIDENCE IF THE THING WOULD HAVE TO BE IN THAT FILE.** The handoff
into this session named one confirmed spec miss — the Movie Inspector's Duration row — quoting
the owner ruling from three documents and a verification at HEAD. **It had been built at phase
14 and was live at `MainWindow.cpp:2276` the whole time**, reading `Duration: 0:05.042 (121
frames)` with origin `encoded`, exactly as the ruling specified. The grep was aimed at
`MovieInspector.cpp`, which **by design holds no fields at all** — it takes a value type, and
every row is built in `MainWindow::buildInspectorSnapshot()`.

**Seventh stale instrument to accuse a correct build**, and the first that is a grep rather than
a measurement. The list is worth keeping whole: phase 8's menu-icon luminance, phase 9's
un-refreshed HUD after the LucidLink probe, phase 10's HUD after a view transform, phase 12's
HUD on resize, phase 14's ANSI-marshalled window titles, phase 15's HUD after a view scale, and
this. **Two of them are the same `update()`-instead-of-`repaint()` mechanism.**

**The mirror-image mistake happened in the same hour and is worth the same weight.** The
inspector's **Audio details** section looked absent from a capture; all five spec fields exist
at `MainWindow.cpp:2501-2530` and the section was simply below the fold of a 739px window. *Read
the code before concluding from a screenshot, and read the screen before concluding from a
grep.*

**ALL N CASES FAILING THE SAME WAY IS A STATEMENT ABOUT THE HARNESS'S INPUTS.** `transitions.ps1
-All` reported `groove or controls not located` on all 25 cases — the exact signature of the
phase 15 window-border fault. It was neither that nor a regression: the invocation **omitted
`-Env TRACE_TRANSPORT_BAR=1`**, which the matrix needs because it locates every control by
scanning the docked bar, and phase 6 took that bar out of the layout by default. The script's own
param block says so in six lines. **Check the invocation against the script header before
building a control binary.**

---

## The regression, as it now stands (physical panel, 5120x1440 @ 239.999Hz)

This is the baseline any future change is measured against, and it is the **first** one taken
across the whole asset set since the interface work began.

| file | cadence | `display` / `win` |
|---|---|---|
| 4K H.264 x3 | **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x | `1226x690 filtered x2` / `1226x1083` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `1226x690 filtered x2` / `1226x1083` |
| 1080p H.264 x2 | **100.0%**, 240 frames, `0 of 239` | `1226x690 filtered x1` / `1226x1083` |
| 4K 60fps x2 | **100.0%**, 162 frames, `0 of 161`, **16.67ms budget** | `1226x690 filtered x2` / `1226x1083` |
| ProRes 422 HQ x2 | **99.9%**, 168 frames, `0 of 167` | `1226x690 filtered x2` / `1226x1083` |
| 1x1 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `690x690 filtered x1` / `690x1083` |
| 4x5 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `552x690 filtered x1` / `552x1083` |

`scrub -SnapRelease` `target 120 shown 120 delta 0` full-res planar, **`hitch 0`**, `land 0` ·
both lifecycle legs (83.6% and the **0% control**) · **25 of 25 transitions** · still and image
sequence both §4-shaped and zero-based · `uiatree.ps1` five named, correctly typed controls on
the drawn rects.

**`handler>budget` is not readable on the 1x1 and the 4x5** — §4 makes those windows narrow and
the dev HUD clips, which the owner ruled a **diagnostic limitation rather than a defect** at
phase 12. Bound it from what is readable rather than quoting it.

---

## Standing priorities (owner) — these outrank anything above

1. **Performance is priority #1.** No feature may ever compromise lightweight, fast, smooth
   playback. If a feature and playback smoothness conflict, the feature loses.
2. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. **Six instances**: the drag preview, §15's scrub
   sampling, accelerated reverse, accelerated forward, phase 4's shuttle-press decision, and
   phase 15's under-resolved preview at a zoom. **Do not re-open any of them on picture-quality
   grounds alone.**
3. **`V:\` is live client production storage and is strictly read-only.**

## Settled behaviour — changing any of these re-opens an owner decision

Nearest magnification above 1:1 (and therefore its point-sampled chroma) · the pan's behaviour ·
Fit to Window taking no default shortcut and staying **enabled while checked** · `kFadeMs`,
`kAutoHideMs` and the 460x84 panel with its 44/34 controls · `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context · Loop persisting across a file change and a restart · the settings home (portable
`trace.ini` beside the exe, else IniFormat under `AppConfigLocation`, **never** `NativeFormat`) ·
the §4 opening-size cap · `d3d11` as the default renderer · the 384MB reverse-cache budget ·
the accessibility proxies staying `Qt::NoFocus` and **out of the tab chain**.

## The rules this project keeps re-learning

**A validated PREDICTION is not a validated MECHANISM.** Phase 14's proxy tree had been written
down and cited for eight phases; building it exactly as described **broke the Space bar**.

**A deferred item's premise expires. Re-derive it before building it.** Nine instances.

**Check what a number is measured against before believing it.** `frames 30 | elapsed 1.25s`
read `100.0% of real time` on a looping file.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Seven times now — see phase 16 above.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**Reproduce on the reported case AND on a healthy one before theorising.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. **`gh` is NOT
  installed**, but the git credential helper holds a usable token — `git credential fill` plus
  `curl` against the REST API is how CI was polled this session.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes from
  **`docs/release-body.md`** via `body_path`, with `generate_release_notes` appending the commit
  log after it. **Rewrite that file when cutting a release, and name the known gaps plainly** —
  an alpha with honest limits gets useful bug reports.
- **CI asserts the renderer initializes** (`--renderer-selftest`, exit 3 = failed to init, exit
  4 = never built) **and the window-shape geometry across DPI** (`--window-shape-selftest`).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. Check the configure
  lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop a running `Trace.exe`
  first** or the link fails with LNK1104.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()` macros**,
  so use `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§` — use `cat` from the Bash tool. **A `git commit -m` here-string containing `>` or `->`
  fails**; write the message to a file and use `git commit -F`. **A bash heredoc containing an
  unbalanced backtick fails the same way** — write the file with the Write tool instead. **Do not
  pipe a measurement script through `Select-Object -First N`** (it raises
  `StopUpstreamCommandsException` and looks like a crash).
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @ 239.999Hz.
  **No subjective smoothness, cadence or picture judgement is valid over Parsec at all.**
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.** `stalls` is
  `2 x refresh`, so the same run reads `stalls 97 … | hitch 0`.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`, `scrub.ps1`
  (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run both legs**),
  `previewshot.ps1`. Most take no `-Clip`: **`restart.ps1` first**.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs `TRACE_NO_AUDIO=1`
  for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `viewscale.ps1`, `inspector.ps1`,
  `uiatree.ps1` (**run under `TRACE_TRANSPORT_BAR=1` as the negative control**), `phase14.ps1`,
  `menushot.ps1`, `recentfiles.ps1`, `resizecache.ps1`, `swapexe.ps1`, `banddiff.ps1`,
  `abfilter.ps1`/`croprect.ps1`, `stalls_vs_window.ps1`, `make_timecode_fixtures.ps1`,
  `make_shape_fixtures.ps1`.
- **Build a control binary in a `git worktree`, not by stashing**, and **verify every swap by
  hash** (`swapexe.ps1`). `windeployqt` the control and copy the `av*`/`sw*` DLLs across.
- Update `CLAUDE.md` and the plans at the end of the session.
