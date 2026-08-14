# OPEN: the bounded async sequential decode queue. FFmpeg checkpoint DONE and CI-green.

**The 8K ProRes 4444 XQ file is NOT signed off and must not be described as solved.** The owner
rejected the frame-drop result on 2026-08-13 — ~11 visible fps reads as visibly poor playback —
and the same file plays perfectly in QuickTime on a *less powerful* macOS machine.
**`TRACE_RT_DROP` is an emergency comparison/fallback only, never the accepted behaviour.**

Acceptance for that file, unchanged and owner-stated:

- full-quality, full-resolution playback, no proxy and no quality reduction;
- correct colour and alpha;
- **every one of the 145 frames presented**;
- sustained **24000/1001**, `drop 0`, `hitch 0`;
- stable cadence with no startup or shutdown flashes;
- exact final frame and clean replay;
- no thermal decline over repeated loops; bounded memory;
- the complete existing regression suite unchanged.

---

## What is DONE: checkpoint 1, the minimal FFmpeg dependency (2026-08-14)

Built, integrated behind its own commits, and **green in CI on a run that compiled it from
scratch** — `Cache not found for input keys: ffmin-…`, not a restored artifact. Commits
`70cb389` · `9f24730` · `24f43ce` · `7c9530e` · `06baa39`.

| | |
|---|---|
| artifact | **20.8 MB** of DLLs (vcpkg 17.3, the rejected BtbN prebuilt 104.3) |
| version | avcodec **62.28.102**, `av_version_info` **8.1.2** — sonames match vcpkg, so it is a drop-in swap |
| licence | **LGPL v2.1 or later**, read from `avutil_license()` on the built binary. No `--enable-gpl`, no `--enable-version3`, **zero `--enable-lib*`** |
| thread default | **`av_cpu_count()` clamped to 64, intra-only only**; long-GOP keeps FFmpeg's automatic count |
| pins | winlibs GCC 16.2.0 (mingw-w64 UCRT 14.0.0-r1) · NASM 2.16.03 · FFmpeg n8.1.2 · msys2 2026-06-11, all SHA256-checked |
| output | **bit-identical** to vcpkg on 4K 4444, 8K XQ, 4448x3096, 422 HQ and H.264 — 0 pixels, max delta 0 |

`scripts/build-ffmpeg/build-minimal-ffmpeg.ps1` builds it; `TRACE_FFMPEG_ROOT` selects it.
**The vcpkg path is untouched and still builds** — the hint is unset by default and deleting one
line of the workflow returns CI to it.

**In Trace, 8K sequential no-drop: `dec 45.08 → 39.68ms`, `handler 74.55 → 69.50ms`, 54.2% →
56.7% of real time, `drop 0`, `hitch 0`.** Regression flat across the validated set.

---

## What is OPEN: checkpoint 2, the bounded async sequential decode queue — NOT STARTED

**The arithmetic that says it closes the gap, from measured in-app numbers:**

- decode (including its demux read) **39.68 ms**
- conversion + upload **30.3 ms**
- serial today: 70 ms = 14.3 fps
- **overlapped: `max(39.7, 30.3)` = 39.7 ms = 25.2 fps**, against a 41.71 ms budget
- with the demux read also inside the decode thread: ~32 ms ≈ **31 fps**

Margin is real but thin at ~2 ms/frame, **so the queue should overlap the read as well as the
decode** — do not assume decode alone is enough.

**Owner requirements, verbatim in substance:**

- ordinary **1× forward playback only** initially;
- overlap demux/read and decode with conversion, upload and presentation;
- **deliberately shallow queue** — a full-resolution 8K 12-bit 4:4:4+alpha frame is ~199 MB;
- report **queue depth, peak working-set increase, allocation behaviour, starvation count and
  high-water mark**;
- reuse buffers where safe; **no unbounded growth**;
- **deterministic cancellation and draining** on pause, stop, seek, scrub, stepping, shuttle,
  file change, end-of-media and shutdown;
- **generation protection** so no stale frame can ever be presented;
- **do not modify** the validated scrub worker, the decoder lease, exact landing, reverse
  playback or frame stepping;
- keep the **synchronous path available** as comparison and rollback control;
- **validate with `TRACE_RT_DROP=0`** — dropping must not hide starvation or missed deadlines;
- measure **demux/read, decode, conversion/upload, queue wait and presentation separately**.
  **Do not infer overlap from the final frame rate alone.**
- its own commit, measurements reported **before** it becomes the default;
- rerun the complete playback, scrub, lifecycle, shuttle and transition regression suite.

**Do not begin CUDA work.** The measurements establish the target is reachable through the
optimised CPU decoder plus pipeline overlap.

**Design constraints already established that the queue must respect:** there is exactly one
`VideoDecoderFFmpeg` and one owner of it at any instant (plan §14's lease); playback currently
decodes synchronously on the UI thread and the scrub worker holds the lease only during a drag;
`supersedeInFlightRequests()` deliberately does **not** tell the scrub worker; and the reverse
shuttle already queues results and pops one per tick — **read `startShuttleRun`/`endShuttleRun`
before designing this, because a bounded sequential forward queue is the same shape and must not
collide with it.**

---

## Working state on the box

- `build/` is vcpkg-configured — the local shipping default. `build-ffci/` is the same tree
  configured with `-DTRACE_FFMPEG_ROOT=C:/tw_ffci/out`, i.e. the minimal FFmpeg. Both pass
  `--renderer-selftest=d3d11`.
- `C:\tw_ffci\out` is the built minimal FFmpeg (bin/, lib/, include/). `C:\tw_bench` holds
  `decbench`, the captures, and **`vcpkg_backup\` — the original vcpkg DLLs**, which is how the
  dependency A/B is reversed without a rebuild.
- The BtbN prebuilt is **the validated performance control only** and must never ship.

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
