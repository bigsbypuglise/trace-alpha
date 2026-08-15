# NO OPEN OWNER DECISION. The 8K ProRes investigation is CLOSED.

Closed by the owner on 2026-08-15 after the decode-thread sweep. **Do not start
stage two, and do not start any new playback-architecture work.** The next
milestone is the owner's to choose; nothing here proposes one.

Everything from the last two sessions is built, measured, committed and pushed.
The working tree is clean.

---

## THE 8K LIMITATION, AS MEASURED

**File:** `12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov` — 7680x4320
ProRes 4444 XQ, `yuva444p12le`, 23.976 fps, 5739 Mbps.
**Machine:** AMD Ryzen 9 5900XT, 16 cores / 32 logical. Physical panel
5120x1440 @ 239.999Hz, `d3d11`, `win 1066x1083`.

**Best full-quality decode and display remains BELOW REAL TIME on this hardware.**

| | fps | % of 23.976 |
|---|---|---|
| Trace, minimal FFmpeg build, default threads, queue off | **13.64** | **56.9%** |
| Trace, same + stage-one queue at depth 2 | **14.87** | **62.0%** |
| Trace, shipping vcpkg build, queue off | 12.72 | 53.0% |

All at full resolution, every frame presented, `drop 0`. No configuration of the
shipped code reaches real time on this file.

### Decode is the binding term, and it is already at its knee

At the knee on the fastest build **`dec` is 39.08 ms against a 41.71 ms frame
budget — 94% of the entire budget consumed by decode alone**, before conversion
(18.01 ms), upload (12.33 ms) or paint. Serial sum 69.4 ms.

**The knee is the logical CPU count and the curve is FLAT beyond it to 64.**
`dec` avg on the 8K plate, vcpkg build: t=1 **661.23** · 8 98.14 · 16 62.29 ·
20 56.90 · 24 52.42 · 28 48.88 · **32 45.95** · 40 45.11 · 48 45.10 · 64 45.12.
Minimal GCC FFmpeg build: 16 55.53 · 24 46.10 · **32 39.08** · 40 39.19.

**CPU never exceeds ~50% of the machine (15.8 of 32 logical cores) at any setting
from 32 upward.** The box is neither thread-starved nor saturated. Past 16 threads
you are on SMT siblings for about 26%; past 32 there is nothing left to ask for.
Amdahl on the t=1/t=32 pair gives a **3.9% serial fraction** whose asymptote is
~26 ms against a measured floor of 45 ms, so **the limit is per-core throughput
and memory traffic, not parallelism.** No thread setting reaches it.

Full sweep with `sws`, `upload`, over-budget counts, drop and CPU per setting:
**`docs/8k-decode-threads-sweep.md`**.

### Stage two is NOT justified by the measured margin and contention

A *perfect* three-stage pipeline — decode on one worker, conversion on a second,
upload and paint on the UI thread — gives `max(39.08, 18.01, ~13.5)` =
**39.08 ms = 25.6 fps at zero contention**. The target is 23.976, so **the margin
is 6%.**

**Stage one measured the contention such an arrangement runs into:** with only
two stages overlapping, `sws` rose **+24%** and `upload` **+91%**. There is no
reason decode is exempt, and a 10% inflation of 39.08 ms is 43 ms — **below 24
fps.** The cost of finding out is a change to the decoder's output boundary
(`VideoFrame` is post-conversion, and `VideoFrame.h` admits no FFmpeg type
because the image-sequence path must compile without FFmpeg).

**Stage one already banked the cheap half — 53.0% → 62.0% — with no boundary
change and a one-line default.** Stage two is a structural change to buy the
expensive half of a gap it probably cannot close.

### Real-time frame dropping is an EMERGENCY FALLBACK, not the solution

`TRACE_RT_DROP` (owner decision, 2026-08-13) holds *media* time on the clock and
drops *picture* when a source cannot sustain its native rate. On this plate in the
shipping configuration that reads **`presented 13.46`, `drop 62`, `media 98.1%`**.

**That is not acceptance of the file and must never be quoted as the 8K result.**
The owner rejected the frame-drop outcome on 2026-08-13 — ~11 visible fps reads as
visibly poor playback. The acceptance bar is unchanged and unmet: full quality,
full resolution, **every frame presented**, sustained 24000/1001, `drop 0`,
`hitch 0`, exact final frame, bounded memory, regression unchanged.

---

## PRESERVE THIS DISTINCTION: standalone decode throughput is NOT presentation throughput

Three different numbers describe "how fast is 8K ProRes here", they differ by
2x, and this project has already been misled twice by conflating them.

| what is being measured | figure | what it includes |
|---|---|---|
| **Standalone FFmpeg decode** (`decbench`, minimal build, t=32) | **25.24 fps** | decode only. No conversion, upload, render, seek or present. Demux subtracted. |
| **Trace's own `dec` term** (in process, minimal build, default threads) | **39.08 ms = 25.6 fps equivalent** | decode only, but inside Trace, on Trace's threads, on Trace's frames. |
| **Trace presentation throughput** | **13.64 fps** | what the user actually sees: decode + conversion + upload + paint, strictly serial. |

**The first two agree within 1.5%, and that agreement is the useful result:
Trace's decoder is not slower than a standalone harness.** The entire gap from
25.6 to 13.64 is the rest of the pipeline being serial — which is exactly what
stage one attacks and partly recovers (→ 14.87).

**Two recorded instances of getting this wrong.** The first 8K ceiling figure in
this repo — "20.5 fps, the machine cannot do it" — was the **winget `ffmpeg.exe`**,
a GPL/GCC statically-linked FFmpeg 9.0 at default threads, substituted for the
LGPL/MSVC libraries Trace links; a different program answering a different
question. And `ffmpeg -f null` made slice-only threading look *faster* on every
ProRes file, which would have closed the intra-only threading question as refuted:
`-f null` decodes and discards, so there is nothing for a frame-threaded decoder to
overlap with. **A benchmark that removes the work your program does around the
thing being measured is measuring a different program.**

---

## SETTLED — do not change any of these without an owner decision

- **Decode thread policy: `av_cpu_count()` clamped to 64 for intra-only codecs;
  FFmpeg's automatic count for long-GOP.** Derived from the machine, never
  hard-coded — a four-core box must get 4 where a literal 32 would break it.
  Long-GOP must keep the automatic count because there `thread_count` is *frames
  in flight* and a deeper pipeline costs a longer refill after every seek. The HUD
  reads **`thr slice x32`** / **`thr frame x16`** so the applied count is
  observable rather than inferred. `TRACE_DECODE_THREADS` remains the override
  and the control.
  **The raised intra-only count also HELPS random access**, which is worth keeping
  because the opposite was plausible: 4K ProRes 4444 `scrub -SnapRelease` at the
  default against `TRACE_DECODE_THREADS=8` reads shuttle **29.63 → 15.89 ms/f**,
  `hitch` **2 → 0**, paints **48 → 84**, with `target 261 shown 261 delta 0` and
  full-res `YUV444P12 planar` on both.
- **The async exact landing stays as it is** (`cc8e638`). A click, a release and a
  frame step decode on the worker. Exactness is unchanged — `RequestMode::Step`,
  one frame, full resolution, accurate conversion, no time budget.
  `TRACE_ASYNC_LANDING=0` is the in-binary control and the rollback.
  **Rapid steps coalesce** — a stated behaviour change, not a side effect.
- **Stage one stays DEFAULT OFF** (`TRACE_PLAYBACK_QUEUE=0`, `d8beba8`). Depth 1
  is worse than off; depth 2 is the minimum that overlaps; the byte budget clamps
  8K to 2 by itself. Turning it on by default is its own owner decision.
- Nearest magnification above 1:1 and its point-sampled chroma · the pan's
  behaviour · Fit to Window taking no default shortcut and staying enabled while
  checked · `kFadeMs`, `kAutoHideMs` and the 460x84 panel with its 44/34 controls ·
  `kMinPlaybackSpeed`, `audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites
  and `frameToRgbImage`'s own swscale context · Loop persisting across a file
  change and a restart · the settings home (portable `trace.ini` beside the exe,
  else IniFormat under `AppConfigLocation`, **never** `NativeFormat`) · the §4
  opening-size cap · `d3d11` as the default renderer · the 384 MB reverse-cache
  budget · the accessibility proxies staying `Qt::NoFocus` and out of the tab chain.

## EXPLICITLY DEFERRED — not cancelled, not started

- **Checkpoint 2 stage two** (three-stage pipeline, conversion on its own thread).
  Designed in `docs/async-decode-queue-design.md`, declined on the measurement in
  `docs/async-decode-queue-stage-one.md` and the sweep above. Re-open only with a
  reason the 6% margin survives contention.
- **Turning stage one on by default.** Measured, safe, worth ~+9% on heavy media
  and nothing on media that meets budget. Needs an owner decision, not more work.
- **Hardware/GPU decode (CUDA/NVDEC).** Explicitly excluded by the owner. It is
  the only remaining lever that could close a 1.6x decode gap, and it is not to be
  begun.
- **The 8K plate's acceptance.** Unmet and now understood; the file is not
  reachable at 23.976 on this machine with this decoder.
- **Step 10, 10-bit display output** — two external gates, unchanged.
- **LucidLink read-ahead** — two designs measured worse; try full-request buffered
  serving before anything else.
- **The 720p ComfyUI subjective comparison against QuickTime** — still not taken,
  and must be **at the machine, not over Parsec**.
- Mixed-monitor DPI residue: three or more displays · 125% and 175% · a scale
  change made while Trace runs on that monitor · hot-plug. §20.3's cross-backend
  band difference at real 150% is still not closed.

---

## The regression baseline (physical panel, 5120x1440 @ 239.999Hz)

| file | cadence | notes |
|---|---|---|
| 4K H.264 x3 | **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x, `drop 0`, `rephase 0` | `thr frame x16` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `thr slice x32` |
| reverse 1x | **100.0%**, 114 frames / 4.75s, `0 of 114`, `hitch 0` | |

`scrub -SnapRelease` `target 261 shown 261 delta 0` full-res planar, `hitch 0`,
`land 0` · **exact paused stepping**: `-StepCycle` landed frame 62 and ended frame
62 through 3 x (Right x5 / Left x5) · both lifecycle legs (**81.8%** and the **0%
control**) · **25 of 25 transitions** · `pq OFF 0/0 … posted 0` proving the queue
inert at its default.

Window geometry here is `win 1066x1083` (transport bar) against an older
`1226x1083`; §22.8's window-size effect applies, so compare like with like.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Thirteen
instances — and the thirteenth was written by this project's own previous session,
which is why the applied thread count is now printed in the HUD rather than
described in a note.

**Standalone throughput is not presentation throughput** (the table above).

**A validated PREDICTION is not a validated MECHANISM.**

**Check what a number is measured against before believing it.** `release` would
have read 0.1 ms on a 595 ms landing; `wait` read 52.01 ms on a run where nothing
waited.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Nine times, two of which *exonerated*
one, which is harder to notice.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a
video file".

**A negative grep is only evidence if the thing would have to be in that file.**

**All N cases failing the same way is a statement about the harness's inputs.**

**Reproduce on the reported case AND on a healthy one before theorising.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify
  with `git remote -v` and `git rev-list --count @{u}..HEAD`. **`gh` is NOT
  installed**; the git credential helper holds a usable token.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body
  comes from **`docs/release-body.md`**.
- **CI asserts the renderer initializes** (`--renderer-selftest`, exit 3 = failed
  to init, exit 4 = never built) **and the window-shape geometry across DPI**
  (`--window-shape-selftest`).
- Build with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. **Stop a
  running `Trace.exe` first** or the link fails with LNK1104. `build/` is vcpkg
  (shipping); **`build-ffci/` is the same tree with
  `-DTRACE_FFMPEG_ROOT=C:/tw_ffci/out`**, the minimal GCC FFmpeg. Both build clean
  and both were measured this session.
- **`windows.h` arrives through the D3D11 backend's header and defines
  `max()`/`min()`**, so use `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it
  mangles every `§` — use the Write tool. **A `git commit -m` here-string
  containing `>` or `->` fails**; write the message to a file and use
  `git commit -F`.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before
  quoting anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is
  5120x1440 @ 239.999Hz. **No subjective smoothness, cadence or picture judgement
  is valid over Parsec at all.**
- **PARK THE MOUSE CURSOR ON THE PRIMARY BEFORE ANY MEASUREMENT.** Windows launches
  a default-positioned window on the monitor the cursor is on. Quote `scr`.
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.**
- The asset set is at **`C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets`**.
- **`scripts/measure/decthreads.ps1`** sweeps `TRACE_DECODE_THREADS` and samples
  process CPU **across the play window only** (cumulative `TotalProcessorTime`
  differenced, reported as cores-busy rather than a percentage of 32).
  **`strip.ps1`** stitches one HUD band from several captures into a labelled
  strip; it loads through a MemoryStream because `Bitmap::FromFile` holds a file
  lock for the life of the object, and a run that errors before `Dispose` leaves
  every source locked so the next run loads nothing and fails in the canvas
  constructor — which reads like a size bug and is not.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`,
  `transitions.ps1` (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**),
  `shuttleland.ps1`, `scrub.ps1` (`-SnapRelease` for anything about the landing),
  `lifecycle.ps1` (**run both legs**), `previewshot.ps1`, `clickland.ps1`,
  `stepcost.ps1`. Most take no `-Clip`: **`restart.ps1` first**, and
  **`widen.ps1` after it on portrait media** or the groove scan fails.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`,
  `overlay_press.ps1`, `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs
  `TRACE_NO_AUDIO=1` for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `widen.ps1`, `viewscale.ps1`, `inspector.ps1`, `uiatree.ps1`, `phase14.ps1`,
  `menushot.ps1`, `recentfiles.ps1`, `resizecache.ps1`, `swapexe.ps1`,
  `banddiff.ps1`, `abfilter.ps1`/`croprect.ps1`, `decthreads.ps1`, `strip.ps1`.

  *Needs two displays at different scale factors*: **`dpimove.ps1`**.
- **Do not run `transitions.ps1` on a 9:16 clip.** Its own header records that
  pillarboxing produces PASSes that mean nothing.
- **Prefer an in-binary control to a control build.** Both changes this session
  shipped with one (`TRACE_ASYNC_LANDING=0`, `TRACE_PLAYBACK_QUEUE=0`), so the two
  runs differ in one branch rather than in a compile. Where a control build is
  unavoidable, use a `git worktree` rather than stashing and **verify every swap
  by hash** (`swapexe.ps1`).
- Update `CLAUDE.md` and the plans at the end of the session.
