# OPEN: two owner decisions, then the decode-queue work they gate.

Both open items were **measured this session and neither was built**, because each turned on a
question only the owner can answer. Read the two decisions first; everything below them is
context.

---

## DECISION 1 — the 4K 9:16 Seedance/ComfyUI scrub report

Full record: **`docs/comfyui-4k-hevc-scrub-measurement.md`** (commit `48af17e`).

**The file has exactly one keyframe — frame 0 — for all 97 frames.** It is HEVC Main 10,
yuv420p10le, 2160x3840, B-frames, 34.2 Mbps; **not** `libx264` and not the class the handoff
assumed. Playback measures **100.0% of real time**, exactly as the owner reported. Every
random-access miss decodes from the head of the file at **8.9 ms a walked frame**, synchronously
on the UI thread for a click, a step and a landing:

| gesture | this file | 4K H.264 control |
|---|---|---|
| click at 30% | **261 ms** frozen | 59 ms |
| click at 60% | **431–520 ms** | 94 ms |
| click at 85% | **585 ms** (`dec 552.50ms`, `walk 82f`) | — |
| backward steps | `2 3 2 4 2 4 2 2 2 **411** 2 2 3 …` | max 3 ms |

**The question for the owner:** *does another player scrub this file well, and is it showing the
exact frame?* It is sharper here than in the previous two reports. Showing frame 57 of a
single-GOP file **requires decoding 58 frames, in any player ever written.** A player that feels
instant is doing one of two things:

1. **decoding off its UI thread**, so the window stays alive and the slider keeps tracking while
   the picture catches up — **Trace can copy this and give up nothing**; or
2. **showing an approximate frame during the gesture** and the exact one on release — a product
   decision. The project has taken that trade six times for frames *in motion*, and **never for
   a click**, which is a landing.

Answer that and the fix follows. Options, ranked, none built:

1. **The exact landing off the UI thread** (roadmap item 2b, already written down). Does not
   make 520 ms shorter; makes it not a freeze. Same ownership machinery as decision 2 — sequence
   them together.
2. **A fill budget proportional to the walk it is keeping**, instead of the constant 18 ms.
   Measured: **+22 ms on a 431 ms landing**, next step freeze from step 11 to step 16. Capped at
   16 entries by the byte budget, so it is a 60% improvement to stepping, not an order of
   magnitude. **More cache bytes is not the answer** (§26.5, 768 MB past the knee).
3. **`skip_frame = AVDISCARD_NONREF` while walking**, restored before the last frames so the
   landing stays exact. Roughly half this file's frames are non-reference. Real upside, most
   invasive, decoder surgery on the one path that must never be approximate.
4. **Ship `TRACE_SCRUB_FILL_MS=240`** — measured here (`hitch 2 → 0`, `smooth max 480.7 → 4.9
   ms`) and recorded as the intended value at §15.2 all along. Re-measure on 4444 and 4K H.264
   first; it is a shared path.

**Do not reach for sampling** (§15's gate is `AV_CODEC_PROP_INTRA_ONLY`; HEVC is not, and one
keyframe makes strided steps worse) **or for the async batch** (`batch cap 4 last 1 max 1` here —
confirmed before anything else was looked at).

**The generalisation that replaces the handoff's:** cost of a miss = **(frames back to the
previous keyframe) × (per-frame decode cost)**, and both terms must be large. 720p ComfyUI
98 × 0.36 ms = 35 ms, harmless; 4K H.264 29 × 2.8 ms = 81 ms, tolerable; **this file 96 × 8.9 ms
= 855 ms.** A keyframe count alone would have condemned the 720p file equally.

---

## DECISION 2 — checkpoint 2 was scoped from arithmetic that puts conversion on the wrong side

Full record: **`docs/async-decode-queue-design.md`** (commit `6d3b5cd`).

The scoping read *decode 39.68 ms · conversion + upload 30.3 ms · overlapped `max(39.7, 30.3)` =
**25.2 fps***. **`convertCurrentFrame` is called from inside `decodeFrameAt`**
(`VideoDecoderFFmpeg.cpp:2185`, `:2200`) and the scrub worker's only decoder call **is**
`decodeFrameAt` (`ScrubDecodeWorker.cpp:217`). Moving decode to a worker **moves conversion with
it** — which is already visible in shipping behaviour, since a drag's `sws` figure comes off the
worker's own perf snapshot.

Measured this session, 8K plate, `TRACE_RT_DROP=0`, vcpkg build, `display 1091x614`:

```
dec 49.33 + sws 17.02 + upload 12.10 = 78.45   against   handler 78.25, outside 2.08ms
handler>budget 88 of 88 (max 87.1) | presented 12.38 / 23.98 fps (51.6%) | drop 0 | thr slice
```

Strictly serial; nothing is waiting on anything. So **one queue stage gives
`max(56.7, 13.2)` = 17.6 fps** on the minimal FFmpeg build against 14.4 today — **+22%, and
short of the 24 the file is accepted at.** Building against 25.2 would have produced a correct
implementation that measured as a failure.

**The 25.2 figure is reachable, by a design nobody has described**: conversion in a **second**
stage — decode N+2 while converting N+1 while uploading N. That changes the decoder's output
boundary (`VideoFrame` is post-conversion, and `VideoFrame.h` admits no FFmpeg type because the
image-sequence path must compile without FFmpeg), and the two stages then contend for the cores
each was measured with alone, against a **~2 ms per frame** margin.

**The question for the owner:** *fund the two-stage pipeline, or take the +22% and accept that
the 8K plate is not reachable this way?* The single stage is worth having either way — it is
real headroom on every file and it is the prerequisite for two.

**The design otherwise stands and does not change with the answer:** a bounded lookahead
**cache** and never a schedule (the tick still picks the frame — `cd79d49` is the scar);
draining **inside `reclaimDecoder()`** so cancellation and every transition the requirements
enumerate is one choke point; `requestGeneration_` stamped on every post and checked at the
delivery boundary in `onScrubResult()`; the lease granted before the first post and returned
only through `reclaimDecoder()`, which `loadCurrentFrame()` already calls; byte-bounded shallow
depth justified by the measured starvation count; **default off**, so the synchronous path is
the comparison and the rollback.

**The 8K acceptance is unchanged and the file is NOT signed off.** Full-quality, full-resolution,
every one of the 145 frames presented, sustained 24000/1001, `drop 0`, `hitch 0`, exact final
frame, bounded memory, regression unchanged. **`TRACE_RT_DROP` is an emergency comparison only,
never the accepted behaviour.** Do not begin CUDA work.

---

## Recommended sequence once both are answered

1. **The exact landing off the UI thread.** It is the fix for decision 1, it is smaller than
   either queue, and it is the same ownership machinery as decision 2.
2. **Single-stage queue**, default off, measured and reported before it becomes the default.
3. **Two-stage pipeline**, only if decision 2 goes that way.

---

## Harness added this session

- **`widen.ps1`** — widens the window without changing the video rect. Portrait media is
  height-bound, so width is pure letterbox and `display` is identical either side (the script
  prints both, so a run carries its own proof). **This is what makes phase 12's clipped-HUD
  limitation cost nothing on 9:16, 4:5 and 1:1 material**, and it is the only reason `scrub.ps1`
  runs on a 9:16 file at all — at the §4 width the groove is under its 300 px minimum run and
  the harness reports `groove not found`. It steps +1/-1 afterwards, because `refreshHud()` is
  not called on `resizeEvent`.
- **`clickland.ps1`** — one groove click, timed from outside as the longest stretch during which
  the window stops answering messages.
- **`stepcost.ps1`** — per-step freeze for N steps in one direction.

**`clickland.ps1`'s first version was a stale instrument and it flattered the build.** A single
`SendMessageTimeout` after the click is serviced ahead of the posted mouse input, so it reported
**3 ms for a landing that went on to block for 450** — consistently enough to "show" that
`TRACE_SEEK_CACHE_WINDOW=16` removed the freeze entirely. It does not. Polling with a 5 ms
timeout and reporting the longest contiguous dead stretch now agrees with the HUD's own
`ui gap max`. **Eighth instrument in this project to accuse or exonerate a build wrongly, and
the first where the wrong answer was the flattering one.**

---

## What is DONE and needs no further work

- **The 720p ComfyUI scrub report** — `docs/comfyui-720p-scrub-measurement.md`, commits
  `efda50c` · `be9f7ec` · `8838c26`. The async chain posted one frame per cross-thread round
  trip; one request now covers up to 4 consecutive frames under the same 8 ms walk budget.
  `TRACE_SCRUB_BATCH=0` is the in-binary control. **Its subjective comparison against QuickTime
  has still not been taken**, and must be **at the machine, not over Parsec**.
- **Mixed-monitor DPI (§20.4)** — closed on hardware 2026-08-14, `8945894` fix · `fb30bb9`
  harness. It found a real bug: §4's sizing never re-ran on a scale-factor change.
  **`v0.2.0-beta.1` is now an owner decision rather than a blocked one.** Still untested, stated
  narrowly: three or more displays · 125% and 175% · a scale change made while Trace is running
  on that monitor · hot-plug. **§20.3's cross-backend band difference at 150% is not closed by
  it.**
- **Checkpoint 1, the minimal FFmpeg dependency** — built, integrated, CI-green from scratch.
  20.8 MB of DLLs, avcodec 62.28.102, LGPL v2.1+, bit-identical output to vcpkg on five files.
  `TRACE_FFMPEG_ROOT` selects it; the vcpkg path is untouched and one deleted workflow line
  returns CI to it.

## Working state on the box

- `build/` is vcpkg-configured (the local shipping default). `build-ffci/` is the same tree with
  `-DTRACE_FFMPEG_ROOT=C:/tw_ffci/out`. Both pass `--renderer-selftest=d3d11`.
- `C:\tw_ffci\out` is the built minimal FFmpeg. `C:\tw_bench` holds `decbench`, the captures and
  **`vcpkg_backup\`**, which is how the dependency A/B is reversed without a rebuild.
- The BtbN prebuilt is the validated performance control only and must never ship.

---

## Standing priorities (owner) — these outrank anything above

1. **Performance is priority #1.** No feature may ever compromise lightweight, fast, smooth
   playback. If a feature and playback smoothness conflict, the feature loses.
2. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. **Six instances**: the drag preview, §15's scrub
   sampling, accelerated reverse, accelerated forward, phase 4's shuttle-press decision, and
   phase 15's under-resolved preview at a zoom. **Do not re-open any of them on picture-quality
   grounds alone.** Note decision 1 asks whether it extends to a *click*, which is a landing and
   has never been covered by it.
3. **`V:\` is live client production storage and is strictly read-only.**

## Settled behaviour — changing any of these re-opens an owner decision

Nearest magnification above 1:1 (and its point-sampled chroma) · the pan's behaviour · Fit to
Window taking no default shortcut and staying **enabled while checked** · `kFadeMs`,
`kAutoHideMs` and the 460x84 panel with its 44/34 controls · `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context · Loop persisting across a file change and a restart · the settings home (portable
`trace.ini` beside the exe, else IniFormat under `AppConfigLocation`, **never** `NativeFormat`) ·
the §4 opening-size cap · `d3d11` as the default renderer · the 384 MB reverse-cache budget ·
the accessibility proxies staying `Qt::NoFocus` and **out of the tab chain**.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Now eleven instances,
and **decision 2 above is the twelfth** — the first where the expired premise was a scoping
arithmetic rather than a note or a line of code.

**A validated PREDICTION is not a validated MECHANISM.**

**Check what a number is measured against before believing it.**

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Eight times now — and the eighth *exonerated* one,
which is harder to notice.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**A negative grep is only evidence if the thing would have to be in that file.**

**All N cases failing the same way is a statement about the harness's inputs.**

**Reproduce on the reported case AND on a healthy one before theorising.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. **`gh` is NOT
  installed**; the git credential helper holds a usable token.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes from
  **`docs/release-body.md`**. Rewrite it when cutting a release and name the known gaps plainly.
- **CI asserts the renderer initializes** (`--renderer-selftest`, exit 3 = failed to init, exit
  4 = never built) **and the window-shape geometry across DPI** (`--window-shape-selftest`).
- Build with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. Check the configure lines
  for `audio output enabled` and `D3D11 renderer enabled`. **Stop a running `Trace.exe` first**
  or the link fails with LNK1104.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()`**, so use
  `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§` — use the Write tool. **A `git commit -m` here-string containing `>` or `->` fails**; write
  the message to a file and use `git commit -F`. **Do not pipe a measurement script through
  `Select-Object -First N`.**
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @ 239.999Hz.
  **No subjective smoothness, cadence or picture judgement is valid over Parsec at all.**
- **PARK THE MOUSE CURSOR ON THE PRIMARY BEFORE ANY MEASUREMENT.** Windows launches a
  default-positioned window on the monitor the cursor is on. Quote the HUD's `scr` field.
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.**
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`, `scrub.ps1`
  (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run both legs**),
  `previewshot.ps1`, `clickland.ps1`, `stepcost.ps1`. Most take no `-Clip`: **`restart.ps1`
  first**, and **`widen.ps1` after it on portrait media** or the groove scan fails.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs `TRACE_NO_AUDIO=1`
  for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `widen.ps1`, `viewscale.ps1`,
  `inspector.ps1`, `uiatree.ps1`, `phase14.ps1`, `menushot.ps1`, `recentfiles.ps1`,
  `resizecache.ps1`, `swapexe.ps1`, `banddiff.ps1`, `abfilter.ps1`/`croprect.ps1`,
  `stalls_vs_window.ps1`, `make_timecode_fixtures.ps1`, `make_shape_fixtures.ps1`.

  *Needs two displays at different scale factors*: **`dpimove.ps1`**. It is the only
  per-monitor-DPI-aware harness here and refuses to run otherwise.
- **Do not run `transitions.ps1` on a 9:16 clip.** Its own header records that pillarboxing
  produces PASSes that mean nothing; `M&M_TopGun_1080.mp4` is the clip for that matrix.
- **Build a control binary in a `git worktree`, not by stashing**, and **verify every swap by
  hash** (`swapexe.ps1`).
- Update `CLAUDE.md` and the plans at the end of the session.

---

## The regression baseline (physical panel, 5120x1440 @ 239.999Hz)

| file | cadence | `display` / `win` |
|---|---|---|
| 4K H.264 x3 | **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x | `1226x690 filtered x2` / `1226x1083` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `1226x690 filtered x2` / `1226x1083` |
| 1080p H.264 x2 | **100.0%**, 240 frames, `0 of 239` | `1226x690 filtered x1` / `1226x1083` |
| 4K 60fps x2 | **100.0%**, 162 frames, `0 of 161`, **16.67ms budget** | `1226x690 filtered x2` / `1226x1083` |
| ProRes 422 HQ x2 | **99.9%**, 168 frames, `0 of 167` | `1226x690 filtered x2` / `1226x1083` |
| 1x1 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `690x690 filtered x1` / `690x1083` |
| 4x5 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `552x690 filtered x1` / `552x1083` |
| **4K 9:16 Seedance (new)** | **100.0% x2**, 96 frames, all 95 gaps ~1x, `handler 3.55/4.43` | `460x818 filtered x3` / `496x1287` |

`scrub -SnapRelease` `target 120 shown 120 delta 0` full-res planar, **`hitch 0`**, `land 0` ·
both lifecycle legs (83.6% and the **0% control**) · **25 of 25 transitions** · still and image
sequence both §4-shaped and zero-based · `uiatree.ps1` five named, correctly typed controls.

**No code changed this session**, so this baseline is carried rather than re-taken; the Seedance
row is new and was measured directly.
