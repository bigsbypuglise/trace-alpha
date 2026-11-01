# MP4 playback and scrubbing — review

**Session of 2026-10-18. Branch `mp4-review`. INVESTIGATION AND MEASUREMENT ONLY —
no shipping code was changed.** Everything added is a harness, a corpus
generator, or this document.

Read the validity section first. One half of the brief's Phase 3 could not be
measured in this session's machine state and is reported as blocked rather than
estimated.

---

## 0. Validity — read before quoting any number

### 0.1 The build is current and the stale FFmpeg is NOT loaded

The brief flagged that `build/app/Release` still holds FFmpeg 7.x DLLs. It does
— `avcodec-61`, `avutil-59`, `swscale-8`, `swresample-5` are all present on disk
beside the current set. **They cannot be loaded**, and that was verified against
the binary rather than reasoned from the build files:

```
$ grep -ao 'avcodec-6[0-9]\.dll' build/app/Release/Trace.exe | sort -u
avcodec-63.dll
$ grep -ao 'swscale-[0-9]*\.dll'  build/app/Release/Trace.exe | sort -u
swscale-10.dll
```

Windows resolves imports by the name in the import table, so an unreferenced DLL
sitting in the directory is inert. The probe was proven able to find something
first (a control search for `Qt6Widgets.dll` hits), per this repo's recorded
`strings`-is-broken trap — `grep -a` on the raw binary is the working method.

`CMakeCache.txt` confirms **Qt 6.11.2** (`C:/Qt/6.11.2/msvc2022_64`) and
**FFmpeg 9.0.1** (`TRACE_FFMPEG_ROOT=C:/tw_ff9/out`,
`FFMPEG_AVCODEC_LIBRARY=C:/tw_ff9/out/lib/avcodec.lib`). A fresh
`--target Trace --parallel` Release build succeeded (exit 0) and every figure
below is from that binary.

The stale DLLs are still worth deleting — they are a trap for the next person
who checks a directory listing instead of an import table — but they are not
affecting anything.

### 0.2 THE DISPLAY WAS WRONG IN BOTH WAYS AT ONCE, SO NO PACING FIGURE WAS TAKEN

Both failures the brief warned about were present simultaneously:

| | reported |
|---|---|
| active display path (`refresh.ps1`) | **1920x1200 @ 59.999 Hz** — a Parsec virtual display |
| the 4090's own panel (`Win32_VideoController`) | **5120x1440 @ 59 Hz** — not 239.999 |
| `parsecd` | **running**, two processes |

`refresh.ps1` describes the *active path* and will happily report the virtual
display without ever saying the panel changed underneath it — which is why
CLAUDE.md's instruction to run `Get-CimInstance Win32_VideoController` beside it
exists, and why both lines are quoted above.

**Consequence, agreed with the owner before measuring:** this session measured
the **display-independent half** of Phase 3 and did not take cadence, drop/repeat
counts, drag latency or smoothness at all. Those are not estimated anywhere in
this document. The reason is not only the remote session — the scrub paint gate's
period is `max(refresh period, 2 x observed paint cost)`, so at 59.999 Hz it is
~17.5 ms against ~4.4 ms at 240 Hz, and a pointer-to-picture figure taken here
would be measuring a different mechanism from the one that ships.

**What that leaves outstanding is listed in section 7.**

### 0.3 mpv is not installed, and for the accuracy half that is an improvement

The brief names mpv `--hr-seek=yes` as the reference. It is not on this machine.
For the **accuracy** half this costs nothing and arguably helps: the reference
used instead is **the frame's own identity burned into its pixels** (section 2),
which is ground truth rather than a second player's opinion. mpv is still wanted
for the **open-time and pacing** comparison, and that is recorded as a gap.

---

## 1. The pipeline, and the thread each stage runs on

Traced end to end for a local MP4. Line numbers are at this branch's HEAD.

### 1.1 Open

| stage | where | thread |
|---|---|---|
| storage classification | `MediaIoSource::open` | caller (UI) |
| custom `AVIOContext` installed | `VideoDecoderFFmpeg.cpp:894` | UI |
| `avformat_open_input` | `:914` | UI |
| `avformat_find_stream_info` | `:929` | UI |
| decoder selection | `:957` `avcodec_find_decoder` | UI |
| thread count / type | `:1014`, `:1064` | UI |
| `avcodec_open2` | `:1069` | UI |
| rate, start ts, frame count | `:1219`, `:1231`, `:1401` | UI |
| first frame | `loadCurrentFrame` → `frameAt` | UI |

Media is read through Trace's own `AVIOContext` rather than FFmpeg's file
protocol, at FFmpeg's own default buffer size, so the read pattern is the one
Trace always had and is merely now visible. Probe limits are left at FFmpeg's
defaults (`TRACE_PROBESIZE` / `TRACE_ANALYZEDURATION` override).

### 1.2 Playback (1x forward)

```
QTimer (deadline-scheduled, re-armed per frame)   MainWindow.cpp:920
  -> audio clock read            :1451  advanceClock()
  -> target frame = seconds*fps  :1452
  -> prepareVideoRequest(Playback)
  -> loadCurrentFrame            :1612  -> :7237
       reclaimDecoder()          :6663  (single choke point)
       frameSource_->frameAt()
         VideoDecoderFFmpeg::decodeFrameAt(Playback)
           [seek decision :2408] [walk] [sws_scale] -> VideoFrame
  -> ViewerWidget::setFrame
       applyColorTransformToRenderer  ViewerWidget.cpp:495
       renderer_->setFrame   (D3D11 upload: 3 planes, or BGRA)
  -> update()
...later in the event loop...
  paintEvent -> D3D11VideoRenderer::paint -> Present(0,0)  D3D11VideoRenderer.cpp:1230
```

**Decode, pixel conversion and GPU upload all run on the UI thread**, inside the
tick handler, synchronously. The paint and `Present` run *after* the handler
returns, later in the same event loop. This is the documented design — an async
prefetch pipeline was built twice and reverted for breaking frame order — and
`handler` on the dev HUD is the measure of it.

**There is no decode queue by default.** `TRACE_PLAYBACK_QUEUE` (checkpoint 2
stage one) is a bounded lookahead *cache* on the scrub worker and ships **off**;
depth 1 is worse than off because it cannot overlap.

### 1.3 Clock

`AudioOutput::advanceClock()` (`AudioOutput.cpp:674`) is the master clock when
and only when `audioShouldDrive()` (`MainWindow.cpp:8891`) is true: there is an
audio track, the media is a video or audio file, the mode is `PlayingForward`,
and the speed is **exactly 1.0** (`abs(speed - 1.0) < 1e-4`). Otherwise the
wall-clock accumulator drives, under GATE E's absolute-deadline scheduler.

The clock runs on wall time and is *disciplined* by the device rather than
sampled from it — `processedUSecs()` advances in device-sized chunks, and reading
it directly made the playhead oscillate. The sink itself is pulled by Qt's own
MMCSS audio thread, not by the UI thread.

### 1.4 Present

`BufferCount = 2`, `DXGI_SWAP_EFFECT_FLIP_DISCARD`, and
`Present(presentSyncInterval, 0)` where the interval is **0** unless
`TRACE_PRESENT_SYNC=1`. So presentation is *not* vsync-throttled and Trace knows
nothing of the refresh phase. There is no `SetMaximumFrameLatency` call.

### 1.5 Threads that exist at all

| thread | created | used for |
|---|---|---|
| UI | — | playback decode+convert+upload, step, exact landings when `TRACE_ASYNC_LANDING=0` |
| `ScrubDecodeWorker` | `ScrubDecodeWorker.cpp:63` | scrub drag chain, reverse shuttle, async exact landing, playback queue when enabled |
| `AudioOutput::DecodeThread` | `AudioOutput.cpp:271` | audio demux + decode |
| Qt WASAPI (MMCSS) | Qt internal | pulls the sink |
| `MediaIoSource::IoWorker` | `MediaIoSource.cpp:113` | **remote sources only** — not created for local files |
| `ParallelBands` pool | `ParallelBands.h` | the CPU colour stage — **shared with EXR**, see 4.9 |

**There is exactly one `VideoDecoderFFmpeg` and one owner of it at any instant.**
The worker takes a *lease*; ownership returns through `reclaimDecoder()`
(`MainWindow.cpp:6663`) alone, which `loadCurrentFrame` and `prepareVideoRequest`
both call — so "the UI thread never touches a leased decoder" is a property of
one function rather than a convention at a dozen call sites.

### 1.6 Seek paths

All four end in `decodeFrameAt`, differing in mode, thread and who picks the
target.

**Drag.** `valueChanged` → `queueVideoScrubFrame` (`:9396`) → bumps the
generation on any target change, then either flushes immediately or lets the
**12 ms** `kScrubCoalesceMs` (`:123`) window pick up the latest pending frame.
`flushVideoScrub` (`:9734`) posts one `ScrubRequest` to the worker covering up to
`TRACE_SCRUB_BATCH` (default 4) **consecutive** frames under an 8 ms budget;
`onScrubResult` (`:9470`) chains the next. Mode `Scrub`. Above 1920 px wide the
conversion is preview resolution (`:1807`); **at 1080p and below it is full
resolution**.

**Click.** Press+release on the same value. The press sets `scrubJumpPending_` so
the first flush lands exactly through `Step` rather than shuttling every frame to
the target; the release is skipped when the press already landed it exactly.

**Release.** `flushVideoScrub(true)` → exact `Step` request. Since `cc8e638` this
runs on the worker (`requestExactFrameAsync`, `:6728`); `TRACE_ASYNC_LANDING=0`
puts it back on the UI thread. Rapid presses coalesce, by design.

**Step ±1 / J-K-L / reverse.** Arrow keys → `stepOneFrame` (`:8050`) → `Step`
mode, exact. The reverse and forward shuttle run on the worker with the commanded
speed as a **stride**, queued, one frame popped per tick.

### 1.7 Exactness mechanism

After any seek, `seekResolvePending` is set and the first decoded frame's index
is **resolved from its PTS** rather than labelled as the target, then the walk
continues forward to the real target. `frameFromPts` (`:1672`):

```cpp
const int64_t relPts = pts - impl_->streamStartTs;
return av_rescale_q_rnd(relPts, impl_->streamTimeBase, frameTb, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
```

with `frameTb = {fpsQ.den, fpsQ.num}` (`:1555`), i.e. **1/fps**. The inverse, in
`seekToFrame` (`:2037`), is `av_rescale_q(target, frameTb, streamTimeBase)`.

**Both directions multiply by the nominal frame rate.** That is the single
assumption the whole index model rests on, and section 4.1 is about what it
costs.

---

## 2. The corpus, and the instrument that reads it

### 2.1 Why a burned-in frame number

The review's central question is whether the frame on screen is the frame that
was asked for. **Every `delta 0` recorded anywhere in this repo is the decoder's
own accounting** — it resolves the landed frame's identity from its PTS, stamps
it on the frame, and compares the stamp with the request. That is a real check
and it caught a real bug. It is also **self-assertion**: if the PTS-to-index
mapping is wrong, the stamp is wrong in the same direction as the comparison and
`delta 0` is reported for the wrong frame. That is exactly the July 2026 scrub bug
— `scrub exact | delta 0` while displaying keyframe 30 for frames 49..55 — and it
survived because the telemetry asserted its own correctness instead of measuring
it.

So the corpus burns the frame number into the pixels:

* **human** — a large `F <n>` and a `pts` timecode, so a wrong landing is visible
  by eye on the panel with no harness at all;
* **machine** — a **10-bit binary strip**, ten boxes, white = 1, MSB left, read
  by thresholding ten sampled cells. No OCR anywhere: OCR on a screenshot is a
  second source of error in front of the measurement.

Generators: `scripts/measure/make_mp4_corpus.ps1` (the encode matrix) and
`make_mp4_corpus_timing.ps1` (the four timing/metadata cases, built by container
surgery rather than by encoding). Reader: `scripts/measure/readburnin.py`.

**The strip holds 10 bits, so a clip must stay under 1024 frames.** The 59.94 fps
row is 14 s rather than 20 s for exactly this reason — at 20 s it is 1199 frames
and frames 1024.. would wrap to 0 and read as a catastrophic landing failure that
is entirely the instrument's.

**The plate sits in the vertical middle of the picture, and that is a
requirement.** At the bottom — where a burn-in naturally goes — Trace's
edge-to-edge 56 px transport strip covered the lower half of it, because the
harness clicks the picture to take foreground and a pointer anywhere inside the
client holds the chrome revealed indefinitely. Middle-left is the one band no
chrome reaches.

### 2.2 The reader was validated, and it was wrong first

Exhaustive: **all 96 frames** of a 4 s clip read correctly, every frame, not a
sample. It also reads correctly at a 6x downscale, which is the size a window
capture actually gives.

**It had a real bug that would have produced fake Trace defects.** The plate
detector's row test required a row to be >80% *grey* across the left half of the
picture. The white bit cells sit on those rows, so how much of the row is grey
depends on **how many bits are set**: frame 13 put two cells in the sampled
window and passed at 81%, frame 42 put three in and failed at 72%. On failure the
detected run collapsed to the plate's bit-free top sliver, ten grey cells were
sampled from above the bits, and the reader returned a confident **`frame 0`**.
Frames 0, 1, 2 and 13 read correctly while 42 and 95 read 0 — a subset with no
obvious pattern, which is exactly what a landing bug looks like.

Two things stop it recurring: the row and column tests now use the same
grey-**or**-white predicate so neither depends on the bit pattern, and the
plate's **aspect ratio is asserted** against the design's 130/1110 so a future
mislocation raises instead of returning a number. That guard then earned itself
immediately on the transport-strip occlusion above — it reported
`plate aspect 0.068 is not the design's 0.117 ... refusing to guess` rather than
a wrong frame.

**Negative controls, all four firing:** no plate → `UNREADABLE`; a frame-13
capture doctored to set the MSB → **525** (= 13 + 512, exactly as predicted); a
grey band of the wrong aspect → the guard; and the undoctored original still
reads 13.

### 2.3 The corpus, measured rather than asserted

Properties read from the containers with `scripts/measure/gopstats.py`, not taken
from the encoder command lines. Keyframes are counted on the **exact** `flags`
field — a loose case-insensitive search for `K` also matches the *k* in
"packet", the trap this repo already records.

| file | frames | keys | max GOP | reorder | pts jitter | pts0 |
|---|---|---|---|---|---|---|
| `h264_1080p_24_allintra` | 480 | 480 | 1 | no | 0 | 0 |
| `h264_1080p_24_gop12` | 480 | 40 | 12 | no | 0 | 0 |
| `h264_1080p_24_gop48` | 480 | 10 | 48 | no | 0 | 0 |
| `h264_1080p_24_gop250` | 480 | 2 | 250 | no | 0 | 0 |
| `h264_1080p_24_gop48_b3` | 480 | 10 | 48 | **yes** | 0 | 0 |
| `h264_1080p_24_opengop` | 480 | 10 | 48 | **yes** | 0 | 0 |
| `h264_1080p_24_422_10bit` | 480 | 10 | 48 | yes | 0 | 0 |
| `h265_1080p_24_gop250` | 480 | 2 | 250 | yes | 0 | 0 |
| `h265_1080p_24_10bit` | 480 | 2 | 250 | yes | 0 | 0 |
| `h264_4k_24_gop48_b3` | 480 | 10 | 48 | yes | 0 | 0 |
| `h265_4k_24_gop250` | 480 | 2 | 246 | yes | 0 | 0 |
| `h264_1080p_23976_gop48` | 480 | 10 | 48 | yes | 0 | 0 |
| `h264_1080p_2997_gop48` | 600 | 13 | 48 | yes | 0 | 0 |
| `h264_1080p_5994_gop48` | 840 | 18 | 48 | yes | 0 | 0 |
| `h264_1080p_vfr` (see 2.5) | 480 | 10 | 48 | yes | **1.0** | 0 |
| `h264_1080p_startoffset` | 480 | 10 | 48 | yes | 0 | **5.0** |
| `h264_1080p_editlist` | 432 | 9 | 48 | yes | 0 | **2.0** |
| `h264_1080p_rot90` | 480 | 10 | 48 | yes | 0 | 0 |

Every fixture carries the property it claims. The rotation one needed a positive
control to establish that: `-show_entries side_data=rotation` returns nothing and
looks like missing metadata, while the correct
`-show_entries stream_side_data=rotation` reads `rotation=90` — confirmed by
running the same query against an iPhone clip known to be rotated
(`rotation=-90`). **The probe was wrong, not the fixture.**

**Open GOP is requested but not independently confirmed.** `-x264-params
open-gop=1` was passed and the file reorders, but nothing in a packet-flags probe
distinguishes an open GOP from a closed one. Treat that row as "open GOP
requested".

### 2.4 Real-world files

From the existing `Trace_Testing_Assets` pool, per the owner's instruction. These
carry **no burn-in**, so they cannot enter the accuracy matrix; they are listed
because their *properties* are what the synthetic rows were chosen to model.

| file | codec | size | r_rate | avg_rate | notes |
|---|---|---|---|---|---|
| `19_IphonePro17max/IMG_0516.MOV` | hevc Main | 3840x2160 | 24000/1001 | **5110/213** | **VFR**, rotation −90 |
| `19_IphonePro17max/IMG_0563.MOV` | hevc Main | 3840x2160 | 24/1 | **229200/9551** | **VFR** |
| `16_4kSeedance_9x16_Comfyui_MP4` | hevc Main 10 | 2160x3840 | 24/1 | 24/1 | **one keyframe, 97 frames** |
| `14_720P_Comfyui_mp4` | h264 High | 1280x720 | 24/1 | 24/1 | 7 keys, irregular GOP |
| `17_Random_Mp4s/*` | h264 High/Main | 720x1280 … 1920x1080 | 24 / 23.976 | same | AI-generated exports |

The two iPhone clips are genuinely variable-rate — `r_frame_rate` and
`avg_frame_rate` disagree — which is what makes the synthetic VFR row a model of
something real rather than a contrived case.

### 2.5 The VFR fixture is not the fixture I set out to build, and the difference matters

`scripts/measure/vfrmap.py` applies Trace's own index arithmetic
(`round((pts - start) * fps)`) to a file's real timestamps and reports what it
costs — a **collision** (two source frames rounding to the same index, so all but
one is unreachable at any index the user can ask for) or a **gap** (an index
inside the range that no frame rounds to). It touches Trace not at all, so it is
an independent prediction rather than a measurement of the thing under test.

Run on the corpus it produced a result that cannot be true as stated, and
checking it is what found the fixture problem:

```
h264_1080p_vfr.mp4     480 frames  404 distinct  76 collide  76 gaps  maxoff 0.002
```

76 lost frames with a maximum offset of 0.002 from the nominal slot are
contradictory — `round()` only moves to a different integer past half a frame.
The timestamps say why:

```
frame  3  pts=0.125000  pts*24= 3.0000
frame  4  pts=0.125000  pts*24= 3.0000     <- same timestamp
frame  9  pts=0.375000  pts*24= 9.0000
frame 10  pts=0.375000  pts*24= 9.0000     <- same timestamp
         (and pts 2560 -> 3584 skips the 3072 slot entirely)
```

**The sub-frame jitter I asked `setpts` for was quantised away.** The expression
offsets each frame by up to 0.35 of a frame period, but the encoder's time base
is 1/24, so pts were rounded to whole frames at encode and then scaled to the
container's 1/12288 — landing on exact 512-tick multiples with **duplicates and
skipped slots** instead of smooth jitter.

So this file tests **duplicate and skipped presentation timestamps**, not
sub-frame variable rate. That is a real-world case in its own right — some
screen-capture and phone pipelines produce it — and its 76 duplicates are
unreachable by *any* player using a time-based index, and ambiguous even to one
using decode order. It is simply not the case its filename implies, and reading
its result as "VFR works" would be wrong.

**The real-world VFR clips are clean, and that is the more useful result.** The
two iPhone files are genuinely variable-rate (`r_frame_rate` and `avg_frame_rate`
disagree) and both map without loss:

```
IMG_0516.MOV   511 frames  511 distinct  0 collide  0 gaps  maxoff 0.310
IMG_0563.MOV   382 frames  382 distinct  0 collide  0 gaps  maxoff 0.040
```

`maxoff 0.310` is the number to carry: **phone-style jitter on this material uses
62% of the half-frame budget the rounding allows.** It maps correctly, with less
than 2x margin. That is a measured headroom figure rather than a reassurance, and
it is why the nominal-rate assumption is listed as a finding rather than as
settled.

**One important caveat on this whole tool: its model of Trace is incomplete, and
section 3.6 is where that is demonstrated.** `vfrmap.py` predicts the
duplicate-timestamp fixture losing 76 frames; the running build loses none,
because the decoder's monotonic bump keeps the index strictly increasing during a
walk and the static arithmetic does not know about it. Use `vfrmap.py` to find
files worth pointing Trace at — it is good at that — and never as a statement of
what Trace will do.

---

## 3. The measurement matrix

`scripts/measure/mp4accuracy.ps1` per file, `mp4matrix.ps1` over the corpus.

### 3.1 What it measures, and why these primitives

Each leg has a **requested frame that is known without measuring anything**:

* `open` — the first displayed frame of a freshly opened file, no navigation.
* `goto` — `Ctrl+G` with a typed frame number. Eight per file: 0, 1, 7, 25%,
  mid+1, 75%, last−1, last.
* `step+` / `step-` — three Right then three Left from mid-file, so the walk is
  exercised in both directions and reversibility is checked.
* `home` / `end` — frame 0 and the container's last frame.

A groove click is deliberately **not** used: its target is geometric, so a
disagreement could be the slider's pixel mapping rather than the decode, and the
measurement could not say which. `Ctrl+G` and an arrow key have no such
ambiguity.

### 3.2 The harness is proven able to fail

* **Live, end to end:** asking for frame 900 on a 480-frame clip reports
  `goto 900 -> displayed 90, FAIL, worst delta 810`. (That is the Go-to-Frame
  **spin box clamping** its input, not a landing bug — a `QSpinBox` with max 479
  rejects "900" and leaves 90. Worth knowing on its own, and it is why targets
  are kept in range.)
* **A matrix with no comparisons can no longer report PASS.** The first full run
  reported `PASS legs=9` on every file — and nine legs is the step, home and end
  legs **with all eight `goto` legs missing**. The default-target expression threw:

  ```powershell
  @(0, 1, 7, [int]($n*0.25), [int]($n*0.5)+1, [int]($n*0.75), $n-2, $n-1)
  ```

  PowerShell's comma operator binds tighter than `-`, so the whole preceding list
  became the left operand of the subtraction and it threw
  `[System.Object[]] does not contain a method named 'op_Subtraction'`; separately,
  `[int]($n*0.5)+1` split into **two** elements, `240` and `1`. The throw left
  `$Targets` at its empty default, the loop iterated zero times, and the run
  called it PASS. **That is this project's "0 of 0" failure reached through
  operator precedence** — a matrix that makes no comparisons finds no mismatches.
  Each target is now computed into its own variable, and the harness **refuses to
  pass with zero `goto` legs**.

### 3.3 Results

Full pool, 17 legs per file, `mp4matrix.ps1`. Every figure is the **burn-in read
from the picture** against the frame that was asked for.

| clip | frames | legs | mismatch | unreadable | worst delta | verdict |
|---|---|---|---|---|---|---|
| `h264_1080p_24_allintra` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_gop12` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_gop48` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_gop250` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_gop48_b3` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_opengop` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_24_422_10bit` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h265_1080p_24_gop250` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h265_1080p_24_10bit` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_4k_24_gop48_b3` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h265_4k_24_gop250` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_23976_gop48` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_2997_gop48` | 600 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_5994_gop48` | 840 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_startoffset` | 480 | 17 | 0 | 0 | 0 | **PASS** |
| `h264_1080p_editlist` | 432 | 17 | 0 | 0 | 0 | **PASS** (see below) |
| `h264_1080p_vfr` | 480 | 17 | 0 | 0 | 0 | **PASS**, and the pass is not the result — see 3.6 |
| `h264_1080p_rot90` | 480 | — | — | — | — | **not readable by this harness** — see 3.5 |

**16 of 18 pass outright on the first run. The other two were harness faults, not
Trace faults, and both are accounted for below.**

### 3.4 The edit list needed its offset declared

The first run reported `mismatch 17, worst delta 48` on `h264_1080p_editlist` —
and the delta was **exactly 48 on all seventeen legs**, which is the shape of a
constant offset rather than a landing error. It is the fixture's own design: the
clip is trimmed 2 s (48 frames) into an already-burned source, so its first frame
carries the burn-in number 48 and `displayed == requested + 48` is **correct**
edit-list handling — Trace maps index 0 to the first frame present.

Re-run with `-BurnInOffset 48` it passes all 17 legs with `worst delta 0`. The
offset is declared by the caller rather than inferred from the first frame, on
purpose: a harness that worked out its own offset would absorb a genuine
48-frame landing bug into it.

### 3.5 The rotation fixture could not be read, for a reason worth keeping

Two harness faults, and the first is a good trap. `ffprobe -of csv=p=0` emits a
**trailing empty field** for a stream carrying side data — a rotated file's
display matrix — so `nb_frames` arrived as `"480,"`, an anchored `^\d+$` test
rejected it, and the target list collapsed to a single frame. The run reported
**4 legs** where every other file reported 17. Fixed by taking the first run of
digits.

The second is not fixable without regenerating the corpus, and **attempting it
made things worse**: a reader that tries the capture at 0/90/180/270 looks like
the obvious answer, and it is wrong, because the bit strip is MSB-first
left-to-right. A 180-degree rotation reverses the bit order and the strip *still
reads as a valid plate* — frame 479 (`0111011111`) came back as **1006**
(`1111101110`). A confident wrong number is strictly worse than an error, and is
the one outcome this instrument exists to prevent. The orientation search was
removed; the reader now refuses a rotated capture.

**The rotation case is nevertheless answered, by a stronger argument than a pixel
read.** `h264_1080p_rot90.mp4` is `-c copy` of `h264_1080p_24_gop48_b3.mp4`, and
their video streams are **byte-identical**:

```
1726e94c…85215  h264_1080p_24_gop48_b3   (elementary stream, SHA-256)
1726e94c…85215  h264_1080p_rot90
2ce76aab…a969b1 h264_1080p_24_gop48      (control: a different file, differs)
pts sequence: 2098d0152f8ee420 for both
```

Same packets, same timestamps, same decoder, same index arithmetic. The only
difference is the display matrix, which Trace reads into `rotationDegrees` and
applies **in the renderer**, downstream of the frame — `rotationDegrees` is read
by neither `frameFromPts` nor `seekToFrame`. The unrotated twin passes all 17
legs, so frame accuracy on the rotated file follows by construction.

Confirmed on screen as well: at `Ctrl+G 241` the HUD reads **`Frame: 241`** with
`dar 0.5625 rot270`. The `rot270` is correct, not a disagreement with the
fixture's `rotation=90` — `av_display_rotation_get` reports counter-clockwise and
Trace stores clockwise, and 360 − 90 = 270.

### 3.6 The VFR file passes, and the pass is the least interesting thing about it

**A PASS on `h264_1080p_vfr` must not be read as "VFR works", for two separate
reasons.**

First, the default target spread passed **by luck**. All eight targets happened
to land on indices that a source frame maps to; `vfrmap.py` predicted that before
the run, which is why a targeted run was done as well.

Second, and more usefully: the targeted run **refuted the prediction**. Asking
for the eight indices that no source frame maps to under the nominal-rate model
returned the right frame in **seven of eight** cases:

```
goto  6 -> 6    goto 12 -> 12   goto 18 -> 18   goto 25 -> 25
goto 31 -> 31   goto 37 -> 37   goto 43 -> 43   goto 50 -> 51
```

The reason is one line that the static model did not account for —
`VideoDecoderFFmpeg.cpp:2184`, inside the decode walk:

```cpp
if (decodedFrame <= impl_->lastDecodedFrame) decodedFrame = impl_->lastDecodedFrame + 1;
```

**The monotonic bump.** When two frames resolve to the same pts-derived index the
second is pushed to `lastDecodedFrame + 1`, so during a forward walk the index
becomes a strictly increasing counter and degenerate timestamps cannot collapse
two frames onto one index. CLAUDE.md records it as preventing frame-order bounce;
it is also what keeps every frame of this file reachable.

**A second hypothesis was then tested and also refuted.** If the index depends on
the keyframe the walk started from, the same index should resolve differently
from different prior positions. Asking for index 50 four times, from 400, from 0,
from 300 and from 49, returned burn-in **51 every time** — stable, not
seek-dependent.

What is actually there is a **transient, self-correcting offset of one frame**:

```
goto 44 -> 44   goto 47 -> 47   goto 48 -> 48   goto 52 -> 52
goto 95 -> 96   goto 96 -> 97   goto 100 -> 100
goto 190 -> 190  goto 240 -> 240  goto 330 -> 330  goto 479 -> 479
```

A walk that starts at a keyframe whose pts-derived index sits one below its
ordinal position hands out indices one low for a few frames, and the offset
disappears as soon as the pts-derived index overtakes the running counter again.
It recurs at some GOP boundaries and not others, and End still lands exactly on
479.

**The honest reading, and it is milder than the one I set out to confirm.** On a
file whose timestamps are degenerate, "the Nth frame" has no unambiguous meaning
in the file itself — two frames genuinely claim the same presentation time — so a
one-frame discrepancy against ordinal position is defensible rather than wrong.
What is fair to say is narrower and still worth saying: **on such a file Trace's
frame index is stable and complete but is not everywhere the frame's ordinal
position, and nothing reports that.** On the sixteen well-formed files it is
exact everywhere.

**Real phone VFR was not tested on screen** — the pool's iPhone clips carry no
burn-in. `vfrmap.py` predicts them clean (0 collisions, 0 gaps on both), with
`IMG_0516.MOV` using 62% of the half-frame rounding budget. That is a prediction,
not a measurement, and closing it needs a burn-in re-encode of a real clip.


---

## 4. Direct answers to the Phase 1 questions

### 4.1 Is hardware decode (D3D11VA) used? If not, why not?

**No. There is no hardware decode anywhere in Trace, and not a single line of
code that could reach it.** A search of the whole tree for `hw_device_ctx`,
`hwaccel`, `hw_frames_ctx`, `d3d11va`, `dxva`, `nvdec`, `cuvid`, `get_format`,
`AV_PIX_FMT_D3D11`, `av_hwdevice_*`, `qsv` and `vaapi` returns **zero hits** in
`src/` and `app/`.

`open()` takes the plain software path: `avcodec_find_decoder(par->codec_id)`
(`VideoDecoderFFmpeg.cpp:957`), no `get_format` callback, no device context. The
only decoder tuning is `thread_count` and `thread_type`.

**Why not** — there is no recorded decision in the repo either way, so the honest
answer is that it was never built rather than that it was rejected. Three
properties of the current design bear on it:

* **The shipping FFmpeg does not obviously exclude it.** The minimal MinGW/GCC
  LGPL build at `C:\tw_ff9` is vcpkg's feature set minus `avdevice`/`avfilter`,
  encoders and muxers. Hardware *decode* is not an `--enable-lib*` matter;
  D3D11VA is a Windows-SDK-backed hwaccel. Whether this particular build has it
  compiled in was **not verified this session** and must be checked before any
  work is scoped — and checked against the **shipped** DLLs, not against the
  gyan build on PATH, which carries `--enable-d3d11va` and is a different binary.
* **GATE C partly collides with it.** Full-resolution frames currently reach the
  GPU as three YUV planes with the matrix applied in the pixel shader. A D3D11VA
  frame is already a GPU texture in NV12, so the win would be larger still (no
  download, no upload) but it is a different path, not a tweak.
* **4:2:2 10-bit is the case that still needs software.** NVDEC cannot decode it,
  which is why that corpus row exists: any hardware path has to keep the software
  one as a per-file fallback rather than replace it.

### 4.2 Are seeks coalesced and cancellable, or can requests queue up?

**Both coalesced and cancellable, by four distinct mechanisms, and nothing
queues.**

1. **Coalescing**, 12 ms. `kScrubCoalesceMs` (`MainWindow.cpp:123`).
   `queueVideoScrubFrame` (`:9396`) responds immediately to the first move, then
   opens the window; moves inside it only update `pendingScrubFrame_`, and the
   timer picks up **the latest** when it fires.
2. **One request in flight.** `postScrubStep` returns early on
   `scrubWorker_.busy()`. Not a depth limit for its own sake — the shuttle's next
   target is derived from the frame last *presented*, so there is nothing
   meaningful to ask for until the outstanding one lands. The chain re-posts from
   `onScrubResult`.
3. **Generation counter, latest target wins.** `requestGeneration_` is monotonic;
   `supersedeInFlightRequests()` (`:6632`) bumps it on **every** change of
   target. `loadCurrentFrame` captures it before decoding and discards the result
   if it moved. There is exactly one such counter in Trace.
4. **Cooperative cancellation inside the GOP walk.** A `CancelPredicate` is
   consulted once per packet at the top of the walk loop — the only point where
   no `AVPacket` is owned, no `AVFrame` is being written and the codec is between
   send and receive. `decodeUntilTarget` returns **three** states, because "the
   frame is not there" and "the frame is no longer wanted" are different things
   and only the first may take the recovery seek.

One deliberate exception worth knowing: `supersedeInFlightRequests()` does **not**
tell the worker. It fires on every pointer move while the shuttle's target does
not move with the pointer, and wiring it through was measured as 111 abandoned
walks and 141 stale results out of 404 posted. The narrow test that does matter
is a *reversal*.

### 4.3 How does an exact-frame seek work? Which flags? Is a keyframe index built?

**Seek to the keyframe at-or-before the target, then decode forward, resolving
the landed frame's identity from its PTS.**

```cpp
// VideoDecoderFFmpeg.cpp:2037
const int64_t relTs    = av_rescale_q(target, frameTb, impl_->streamTimeBase);
const int64_t targetTs = impl_->streamStartTs + relTs;
av_seek_frame(impl_->fmt, impl_->streamIndex, targetTs, AVSEEK_FLAG_BACKWARD);
avcodec_flush_buffers(impl_->codec);
impl_->lastDecodedFrame   = -1;     // position now unknown
impl_->seekResolvePending = true;
```

* **`av_seek_frame` with `AVSEEK_FLAG_BACKWARD`** — not `avformat_seek_file`, and
  no `AVSEEK_FLAG_ANY`.
* **No keyframe index is built on open.** Trace relies on the container's own
  index — for MP4 the `stss`/`stts` tables the demuxer reads. Nothing in the
  decoder calls `av_index_*` or walks the stream to build one.
* **The landed frame is never labelled as the target.** `seekResolvePending` makes
  the first decoded frame's index come from `best_effort_timestamp` through
  `frameFromPts` (`:1672`); the walk then continues to the real target. This is
  what `e76eabb` added, and its absence is what the July 2026 scrub bug was.
* **The seek decision** is one expression, `needSeek` (`:2408`): no known
  position, the decoder cannot reach the target by walking forward, a backward
  request, a `Step` jump of more than 1, a re-request of the current frame outside
  playback, or a non-sequential forward gap. `decoderCanReachByWalking` tests
  `lastDecodedFrame`, **not** `currentFrame_` — a cache hit moves the latter
  without advancing the decoder, and conflating the two is a bug this project has
  already fixed once.

### 4.4 Is there a fast/coarse scrub mode during drag, with exact refinement on release?

**Yes, and it is four separate reductions rather than one mode.**

| during drag | on release |
|---|---|
| preview-resolution conversion **above 1920 px only** (`:1807`) | full resolution |
| fast swscale flags (`mode != Step`) | `SWS_FULL_CHR_H_INT` + `SWS_ACCURATE_RND` |
| frames may be **sampled** on intra-only, and on long-GOP backward since 2026-08-20 | every requested frame |
| paint gated to `max(refresh, 2x paint cost)` | painted |

**At 1080p and below there is no preview downscale at all.** The threshold is
`> 1920`, because at exactly 1080p halving is a pessimisation — a full-res
convert is unscaled, while halving adds a 1920 to 960 resample costing more than
the smaller output saves. So a 1080p drag converts every frame at full
resolution.

The release is an exact `Step` request and is the only path whose fidelity is
owed. That split is the owner's standing rule: smooth scrubbing takes priority
over matching final-frame quality *during motion*, and fidelity is owed to the
frame the user stops on.

### 4.5 How are frame numbers mapped to PTS?

**`frame index = round((pts - stream_start_time) * fps)`, and the inverse — both
directions multiplying by the nominal rate.** `frameTb` is `1/fps` (`:1555`); the
rate is `av_guess_frame_rate` (`:1219`), kept as a rational.

Case by case:

* **`time_base`** — handled. Both directions go through `av_rescale_q*` between
  the stream's own time base and `1/fps`.
* **B-frame reordering** — handled, and correctly rather than incidentally: the
  index comes from `best_effort_timestamp`, which is presentation order, not from
  decode order or a counter.
* **Non-zero start time** — handled. `streamStartTs` is `stream->start_time`
  (`:1231`), subtracted on the way in and added on the way out.
* **Edit lists** — handled by FFmpeg's MOV demuxer rather than by Trace, which
  sees whatever timestamps the demuxer presents. Measured in section 3.4.
* **Rotation** — read from the display matrix via `av_display_rotation_get`,
  snapped to a quarter turn, with `rotationSnapped` recording that it was. It is
  **orientation only**: pixels are decoded as encoded and the rotation is applied
  in the renderer. It does not touch the index mapping.
* **VFR — NOT handled, and this is the structural finding.** `av_guess_frame_rate`
  returns a single nominal rate, so on variable-rate material "frame N" means
  "the frame whose timestamp is nearest N/fps" and **not** "the Nth frame in the
  file". The two directions are mutually consistent, so nothing contradicts
  itself internally — but two source frames can round to the same index (one then
  being unreachable), some indices can map to no frame, and `frameCount` comes
  from `stream->nb_frames` (`:1401`), the **true** count, which therefore
  disagrees with the reachable index range. The audio-clocked playback path makes
  the same assumption independently at `MainWindow.cpp:1452`
  (`audioFramePos = audioSeconds * fps`). Measured in section 3.6.

### 4.6 What does the reverse cache cover? Does scrubbing use it?

**It is not a reverse cache — it is a general random-access frame cache, and
scrubbing uses it in both directions.**

`TRACE_REVERSE_CACHE_MB`, default **384 MB**, bounded **by bytes** rather than by
entry count, evicted FIFO. The consult is:

```cpp
// VideoDecoderFFmpeg.cpp:2306
const bool randomAccess = (mode != RequestMode::Playback) && !requestIsSequentialForward;
if ((requestIsBackward || randomAccess) && tryReverseCache(frameIndex)) { ... }
```

So **any** non-sequential request outside playback hits it, forward or backward.
Sequential playback is deliberately excluded — it must keep advancing the decoder
rather than being served from behind.

It is filled by the presented frame plus the frames walked past en route to a
target (budget `TRACE_SCRUB_FILL_MS`, **60 ms** in force — the member initialiser
says 240 and `open()` overwrites it, a documented code-versus-comment
discrepancy). Preview-resolution entries are **tagged** and refused for anything
but a `Scrub`, so a soft frame can never be served as a landing.

Two structural notes. On intra-only media a seek lands directly on the target, so
no intermediate frames are ever produced and **there is nothing to cache** — do
not try to fix ProRes hit rates with more bytes. And on the single-keyframe
Seedance clip a full-res entry is 23.7 MB, so 384 MB holds 16 frames of 97.

### 4.7 Everything that touches the UI thread on the playback or seek path

| on the UI thread | notes |
|---|---|
| **playback decode** | `loadCurrentFrame` at `MainWindow.cpp:1612`, synchronous, every frame |
| **pixel conversion** | `convertCurrentFrame` is called *from inside* `decodeFrameAt`, so it rides with decode wherever decode runs |
| **GPU upload** | `renderer_->setFrame` inside `ViewerWidget::setFrame` |
| **the CPU colour stage** | when a transform is active — see 4.9 |
| **frame step +-1** | `stepOneFrame` to `Step`, synchronous |
| **the synchronous scrub walk** | only under `TRACE_ASYNC_SCRUB=0` |
| **the exact landing** | only under `TRACE_ASYNC_LANDING=0`; on the worker by default |
| **`avformat_find_stream_info`** | the dominant term in open time |
| **paint and `Present`** | after the tick handler returns, later in the event loop |

| **not** on the UI thread | |
|---|---|
| scrub drag chain, reverse and forward shuttle, exact landing | `ScrubDecodeWorker` |
| audio demux and decode | `AudioOutput::DecodeThread` |
| the sink pull | Qt's WASAPI MMCSS thread — **not** the UI thread |
| remote file reads | `MediaIoSource::IoWorker`, and **only for remote sources** |

Two corrections to premises in the brief. **Qt does not pull the `QAudioSink`'s
`QIODevice` on the UI thread** on this Qt: 6.10+'s Windows backend is
`QWASAPIAudioSinkStream`, and `Qt6Multimedia.dll` carries
`AvSetMmThreadCharacteristics` plus `"Pro Audio"` — a dedicated MMCSS audio
thread. That was measured in a previous session and is why the window-drag
dropout investigation closed as refuted. **And the synchronous-EXR-load precedent
does not transfer**: that is the image-sequence path. The MP4 path's synchronous
work is decode, conversion and upload, listed above.

One re-entrancy guard is worth knowing: `storageBusy_` exists because a slow
remote read pumps the event loop, which can deliver a timer tick or a key press
*inside* a decode. Every path that drives the decoder checks it.

### 4.8 Which TRACE_* escape hatches affect MP4, and is any masking a structural problem?

91 `TRACE_*` names exist in the tree; these are the ones on the MP4 path.

**Default-on mechanisms with an off switch** — the switch is the control, the
mechanism ships:

| knob | off means | masking anything? |
|---|---|---|
| `TRACE_ASYNC_SCRUB=0` | synchronous UI-thread walk | no |
| `TRACE_ASYNC_LANDING=0` | landing on the UI thread | no |
| `TRACE_SCRUB_BATCH=0/1` | one frame per request | no |
| `TRACE_SCRUB_PAINT_GATE=0` | paint every delivered frame | **arguably — see below** |
| `TRACE_DEADLINE_SCHED=0` | the old integer tick | no |
| `TRACE_PLANAR_UPLOAD=0` | swscale to BGRA | no |
| `TRACE_GPU_REDUCE=0` | single bilinear tap | no |
| `TRACE_REVERSE_ASYNC=0` | synchronous reverse | no |
| `TRACE_SCRUB_GOP_SAMPLE=0` | long-GOP never samples | no |
| `TRACE_RT_DROP=0` | never drop a playback frame | **yes, and deliberately** |
| `TRACE_SCHED_FREERUN_LATE=0` | arm for the next grid slot | no |

**Default-off, i.e. not shipping:** `TRACE_PLAYBACK_QUEUE` (the decode queue),
`TRACE_IO_READAHEAD`, `TRACE_PRESENT_SYNC`, `TRACE_INTRA_FRAME_THREADS`,
`TRACE_LONGGOP_SLICE_THREADS`, `TRACE_SCRUB_PACE`.

**Tuning values:** `TRACE_DECODE_THREADS` (0 = FFmpeg's automatic for long-GOP,
`av_cpu_count()` for intra-only), `TRACE_REVERSE_CACHE_MB` (384),
`TRACE_SCRUB_FILL_MS` (60), `TRACE_SEEK_CACHE_WINDOW`, `TRACE_PROBESIZE`,
`TRACE_ANALYZEDURATION`.

**The two that are masking rather than fixing:**

* **`TRACE_RT_DROP`** — the real-time frame drop holds *media* time by dropping
  *picture* when a source cannot sustain its rate. It is an explicit owner
  decision, a bounded and documented exception to "never skip a frame", and inert
  on everything that meets budget (`drop 0` across the validated set). But it
  means "plays at real time" and "shows every frame" are different claims for any
  file heavy enough to engage it, and the HUD's `media %` against `real time %`
  is the only place that distinction is visible. Not a defect — but it is the
  mechanism that would make a slow MP4 *look* fine.
* **`TRACE_SCRUB_PAINT_GATE`** — the gate fixed a real fault on a machine class
  where presents block at the refresh (the Threadripper report), and on that
  class it is the difference between a hitching drag and a merely slow one. What
  it does *not* address is why the per-frame work exists on the files that need
  it; it decouples painting from decoding so the thread is not saturated by
  paints. That is the right fix for the fault it addresses, and it is also what
  would make a decode-bound file feel acceptable without its decode cost
  changing.

Neither should be removed. Both are worth naming because a future "MP4 playback
is fine now" claim can be true with either of them doing the work.

### 4.9 Shared code with the EXR/OCIO path — flagged per the brief

Three places where MP4 and EXR share code, so a change to one reaches the other:

1. **`ColorTransform`** is applied in `ViewerWidget::applyColorTransformToRenderer`
   (`ViewerWidget.cpp:495`) for **both** media classes. It holds two CPU
   processors — `uint8 -> uint8` is the video path, `f32 -> uint8` the EXR path —
   so the processors are separate but the seam is not.
2. **`ParallelBands`** (`src/core/ParallelBands.h`) is the thread-band helper used
   by `ColorTransform` and `DisplayMapping`, spinning up `std::thread`s per frame
   band. On the MP4 path it only runs with a transform active.
3. **`syncPlanarOutput`** (`MainWindow.cpp:6433`) — **an active colour transform
   turns GATE C off for MP4**: `allowed && !colorTransform_.isActive() &&
   rendererAcceptsPlanarYuv()`. So loading a LUT on a video file reverts
   full-resolution frames to swscale-to-BGRA *and* adds a per-frame CPU colour
   stage. Measured previously at 4K H.264: `handoff 0.76 ms` off against
   13.27–14.13 ms on in parallel bands, with single-threaded at 77.28 ms.
   **Nothing in this session's figures has a transform active.**

The frame cache, the decoder, the scrub worker and the scheduler are **not**
shared with the EXR path — that path has its own loader and its own window cache.

---

## 5. Findings

Ranked by impact on **consistency** — the brief's actual target — not by how
interesting they are. Each carries its evidence, a proposed fix, how to measure
the fix, and the risk.

### F0. The exactness contract holds, and it now holds against an external check

**This is the headline and it is a positive result.** Absolute landing accuracy
is exact — `displayed == requested`, delta 0, read from the pixels rather than
from Trace's own accounting — across every constant-rate file in the corpus:
GOP 1, 12, 48 and 250; 0 and 3 B-frames; open GOP; 4:2:0 8-bit, 4:2:2 10-bit and
4:2:0 10-bit; H.264 and H.265; 1080p and 4K; 23.976, 24, 29.97 and 59.94 fps; a
non-zero stream start time; and an MP4 edit list. On `open`, on eight `Ctrl+G`
landings per file including the first, last and mid-GOP frames, on three forward
and three backward single steps with reversibility, and on Home and End.

Every `delta 0` previously recorded in this repo was the decoder comparing its
own PTS-derived stamp against its own request. That check is real, but if the
mapping is wrong the stamp is wrong in the same direction — which is exactly how
the July 2026 scrub bug reported `delta 0` while showing keyframe 30 for frames
49..55. **The mapping is now confirmed from outside it.** Nothing below detracts
from that; the findings are about the cases the corpus could reach that CFR
material cannot.

### F1. No hardware decode exists, and software decode is on the UI thread

> **CLOSED 2026-10-31 BY MEASUREMENT, AND THE PREMISE BELOW IS HALF WRONG.
> Record: `docs/mp4-hwdec-f1.md`. Read that first; what follows is the record of
> what was proposed, not of what is true.**
>
> D3D11VA decode is built behind `TRACE_HWDEC=d3d11va`, default off, and the
> recommendation is that it stays off. Its pixels are bit-identical to software
> decode (0 of 43127 px, max channel delta 0, under a controlled substitution of
> the conversion route), and it costs **~19ms per 4K frame**: on the one pool
> clip with no headroom — 4K H.264 at 60fps — it goes from **100.0% of real time
> to 63.8% with `drop 57`**.
>
> **Why the premise fails: decode is DISPATCHED on the UI thread and PERFORMED
> on frame-threading workers.** The tick pays the handoff, not the decode —
> `dec` averages **0.37ms** on 4K H.264 at 60fps and **0.39ms** on 4K HEVC, with
> the whole handler at 2.33ms of 41.71ms. There was nothing to win.
>
> **And it is structural: every pool file that misses its budget is ProRes, and
> ProRes has no D3D11VA path at all.** The hardware decoder covers exactly the
> class that does not need it. Zero-copy is recommended against as well, with
> its ceiling computed in §6 of the record.

**Impact: this is the ceiling on every heavy MP4, and it is a consistency
problem rather than only a speed one.**

* **Evidence.** Zero hits across `src/` and `app/` for every hardware-decode
  symbol (4.1). `avcodec_find_decoder` at `VideoDecoderFFmpeg.cpp:957` with no
  `get_format` and no device context. Playback decode, pixel conversion and GPU
  upload all run inside the tick handler on the UI thread (4.7, 1.2) — and
  conversion is not separable from decode, because `convertCurrentFrame` is
  called from *inside* `decodeFrameAt`.
* **Why it is a consistency problem.** A file whose per-frame cost exceeds the
  budget does not degrade gracefully in one dimension — it stalls the interface,
  and the two mechanisms that absorb that (`TRACE_RT_DROP` holding media time by
  dropping picture, and the scrub paint gate decoupling paints from decodes) make
  the *result* smooth while the *cost* is unchanged. So "which files play
  properly" is decided by a per-frame cost nobody can predict from the file's
  properties, which is the opposite of the brief's target.
* **Fix.** D3D11VA decode with the software path kept as a per-file fallback.
* **How to measure it.** `decbench` for decode in isolation, then the `handler`
  term in Trace — and **the handler, not the frame rate**, because the recorded
  scar here is that `ffmpeg -f null` made slice-only threading look faster on
  every ProRes file while Trace measured the opposite: a benchmark that removes
  the work your program does around the thing being measured is measuring a
  different program.
* **Risk, and it is substantial.** A D3D11VA frame is a GPU texture in NV12,
  which is a different frame path from GATE C's three-plane upload rather than a
  tweak to it. 4:2:2 10-bit cannot be hardware-decoded by NVDEC at all, so the
  software path must stay and the two must agree on colour. **And the first thing
  to establish is whether the shipped minimal LGPL FFmpeg has D3D11VA compiled in
  at all** — checked against the shipped DLLs, not the gyan build on PATH, which
  has `--enable-d3d11va` and is a different binary. If it does not, this is a
  toolchain change as well, and the vcpkg pin does not move backwards.

### F2. The frame-index model assumes a constant frame rate — and the measurement made this milder than the code reading did

**Impact: on a file whose presentation timestamps are degenerate, Trace's frame
index is not everywhere the frame's ordinal position, and nothing reports it.
Frames are NOT lost.**

This finding is stated at the width the measurement supports, which is narrower
than where it started. Recording the correction rather than only the conclusion,
because the first version of it was the kind of claim that sends a session after
the wrong thing.

* **The code reading.** `frameFromPts` (`:1672`) and `seekToFrame` (`:2037`) both
  multiply by the nominal rate from `av_guess_frame_rate` (`:1219`), so "frame N"
  means "the frame whose timestamp is nearest N/fps" rather than "the Nth frame".
  The audio-clocked playback path makes the same assumption independently at
  `MainWindow.cpp:1452` (`audioFramePos = audioSeconds * fps`).
* **The static prediction, which was wrong.** `vfrmap.py` applies exactly that
  arithmetic to a file's real timestamps and predicted the duplicate-timestamp
  fixture losing **76 of 480 frames** to index collisions, with 76 indices no
  frame maps to.
* **What the running build actually does.** It loses none. Seven of the eight
  predicted-unreachable indices returned the right frame, because of one line the
  static model did not account for — the monotonic bump at `:2184`:
  `if (decodedFrame <= lastDecodedFrame) decodedFrame = lastDecodedFrame + 1`.
  During a walk the index is therefore a strictly increasing counter, and two
  frames sharing a timestamp cannot collapse onto one index. CLAUDE.md records
  that line as preventing frame-order bounce; it is also what makes degenerate
  timestamps survivable.
* **A second hypothesis, also refuted.** If the index depended on which keyframe
  the walk started from, the same index would resolve differently from different
  prior positions. Index 50 returned the same frame four times, from 400, from 0,
  from 300 and from 49. It is stable.
* **What is left, measured.** A **transient one-frame offset** that self-corrects
  within a GOP: `48 → 48`, `52 → 52`, but `95 → 96`, `96 → 97`, `100 → 100`, and
  End still lands exactly on 479 (3.6). A walk starting at a keyframe whose
  pts-derived index sits one below its ordinal position hands out indices one low
  until the pts overtakes the counter again.
* **And a point in Trace's favour that should not be lost:** on a file where two
  frames genuinely claim the same presentation time, "the Nth frame" has no
  unambiguous meaning *in the file*. A one-frame discrepancy against ordinal
  position there is defensible, not wrong.
* **The real gap is that real-world VFR was never measured on screen.** The
  pool's two iPhone clips are genuinely variable-rate and carry no burn-in.
  `vfrmap.py` predicts both clean — 511 frames, 511 distinct indices — with
  `IMG_0516.MOV` using **62% of the half-frame rounding budget** (`maxoff 0.310`
  against 0.5). That is the number to watch: it is a prediction, it has less than
  2x margin, and the fixture that was supposed to probe it turned out to test
  something else (2.5).
* **Fix, if one is wanted.** Index from the container's own frame table rather
  than a nominal rate: build the PTS list at open and make "frame N" the Nth
  entry. It would make the index ordinal by construction and collapse F3 with it.
* **Risk.** Open time — building a PTS index needs the packet list, which is
  already what `avformat_find_stream_info` dominates. It also changes what "frame
  N" means on any file where the two models differ, so timecode, Go to Frame and
  every recorded figure move. **Given the measured severity, this is not
  obviously worth doing yet** — which is why step 1 of the build order is to
  measure real phone footage before deciding.

### F3. `frameCount` and the reachable index range are separate quantities — theoretically

**Demoted after measurement, and kept only so nobody rediscovers the code path
and assumes it bites.**

* **Evidence.** `metadata_.frameCount` is `stream->nb_frames` when the container
  states it (`:1401`), i.e. the true frame count. `VideoFrameSource::maxFrame()`
  returns `frameCount - 1` (`VideoFrameSource.cpp:54`) and `PlaybackController`
  clamps the playhead to it (`PlaybackController.cpp:127`). Nothing checks that
  the index arithmetic can actually reach that value.
* **Measured impact: none found.** The monotonic bump (F2) makes the walk hand
  out one index per frame, so on the degenerate-timestamp fixture all 480 frames
  occupy indices 0..479 and End lands exactly on 479 — not luck, as an earlier
  draft of this section claimed, but the bump doing its job.
* **It remains a real hazard in principle**, because the two numbers are computed
  by unrelated means and nothing asserts they agree. A file where the first
  frame's pts-derived index is far from 0 would expose it.
* **Fix.** Falls out of F2 if F2 is ever done. **Not worth doing on its own**, on
  this evidence.

### F4. Playback has no pipeline, and the one that was measured returned a third of its prediction

* **Evidence.** Decode, conversion and upload are strictly serial on the UI
  thread (1.2). `TRACE_PLAYBACK_QUEUE` ships **off**, and depth 1 is worse than
  off because a depth-1 queue cannot overlap. Stage one measured **+10% against a
  predicted +22%** — because conversion rides with decode, so moving decode to a
  worker moves conversion with it, and the two overlapped stages then contend
  (`sws` +24%, `upload` +91%).
* **Fix.** The two-stage pipeline already designed in
  `docs/async-decode-queue-design.md`.
* **Risk.** The margin is thin and already measured as thin. Do the arithmetic on
  the real decomposition before building: the recorded lesson is that building
  against the wrong arithmetic produces a correct implementation that measures as
  a failure.
* **Note.** If F1 lands, this changes shape entirely — a hardware-decoded frame
  does not need the conversion stage at all.

### F5. No keyframe index is built, so seek quality is the container's

* **Evidence.** Nothing in the decoder calls `av_index_*` or builds a table;
  `av_seek_frame` uses the demuxer's own index, which for MP4 is `stss`/`stts`
  (4.3).
* **Impact.** Correct and cheap for well-formed MP4. **Not measured** for a file
  with a damaged or absent index, which is a real shape among the AI-generated
  and screen-capture exports this review is partly about.
* **Fix.** None proposed — relying on the container's index is right. What is
  missing is a *test*: a deliberately index-damaged MP4 in the corpus.

### F6. An active colour transform silently disables GATE C for video

* **Evidence.** `syncPlanarOutput` (`MainWindow.cpp:6433`):
  `allowed && !colorTransform_.isActive() && rendererAcceptsPlanarYuv()`. So
  loading a LUT on an MP4 reverts full-resolution frames to swscale-to-BGRA *and*
  adds a per-frame CPU colour stage in parallel bands, shared with the EXR path
  (4.9).
* **Impact.** A reviewer with a LUT loaded is on a measurably different playback
  path from every figure recorded in this repo, and nothing on screen says the
  planar path stood down. Measured previously at 4K H.264: `handoff` 0.76 ms off
  against 13.27–14.13 ms on.
* **Fix.** The GPU colour stage (EXR stage 5) removes the reason. Cheaply and
  immediately: put the planar stand-down on the HUD beside `planar`, so the
  configuration is readable rather than inferred — this codebase's own rule for
  `renderer`, `font` and `strip`.
* **Risk.** Low for the HUD field. The GPU stage is its own phase.

### F7. Two smaller items, both real and both cheap

* **`scrubWalkCacheBudgetMs` disagrees with its own comment.** The member
  initialiser is `240.0` (`:573`) and `open()` overwrites it unconditionally with
  `envInt("TRACE_SCRUB_FILL_MS", 60)` (`:1174`), on every media open. **60
  ships.** CLAUDE.md already records this as a documentation-versus-code
  discrepancy; it is still there. Fix the initialiser or the comment, and pick
  deliberately — the figure has never been swept since the landing moved to the
  worker.
* **Go to Frame clamps out-of-range input silently.** Typing 900 on a 480-frame
  clip lands on **90**: the `QSpinBox` validator rejects the third digit and
  leaves what it had. No message, and the playhead moves somewhere the user did
  not ask for — which is the behaviour the Go To prompts' own validation rule was
  written to avoid ("validate before seeking and refuse rather than clamp"). The
  rule is implemented for the *typed-text* Go to Timecode and bypassed by the
  spin box's own editing behaviour in Go to Frame.

### F8. What did NOT turn out to be a problem

Worth recording so nobody re-investigates:

* **B-frame reordering** — correct, and correct by construction: the index comes
  from `best_effort_timestamp`, i.e. presentation order. Exact on both B-frame
  rows of the corpus, including on backward single steps.
* **Non-zero stream start time** — correct. `streamStartTs` is subtracted and
  added symmetrically; the 5-second-offset fixture lands exactly on all 17 legs.
* **MP4 edit lists** — correct. The trimmed fixture reports `displayed =
  requested + 48` consistently on every leg, which is the burn-in carrying source
  numbering while Trace correctly maps index 0 to the first frame present.
* **Seek coalescing and cancellation** — four mechanisms, nothing queues (4.2).
  No evidence of request pile-up anywhere.
* **Qt pulling the audio sink on the UI thread** — it does not, on this Qt
  (4.7). A premise in the brief; refuted in a previous session and re-confirmed
  here from the DLL's own imports.
* **The stale FFmpeg 7.x DLLs** — present but not imported, so inert (0.1).


---

## 6. Recommended build order

Nothing here is started. The ordering is by **what unblocks or invalidates what**,
not by size.

### Step 0 — restore the measurement environment (not optional, and not code)

Panel to 5120x1440 @ 239.999 Hz, Parsec closed, verified with `refresh.ps1`
**and** `Get-CimInstance Win32_VideoController` together. Then re-run the pacing
half of Phase 3 that this session could not take (section 7). **Everything below
is scoped against per-frame cost figures, and this session has none that are
valid** — so any of steps 2–4 planned on today's numbers would be planned on the
wrong numbers.

Install mpv while there, for the open-time and pacing reference.

### Step 1 — close the measurement gaps the corpus still has (cheap, no shipping code)

The targeted VFR run named here in an earlier draft was **done in this session**
and is section 3.6; it refuted its own prediction, which is why F2 is now stated
narrowly. What remains:

1. **A real-world burn-in clip — the highest-value item on this list.** The
   pool's iPhone footage is genuinely variable-rate and carries no burn-in, so
   the one VFR case that matters has never been on screen. Re-encoding one with
   the strip over it keeps its GOP and timing shape while making it measurable.
   `IMG_0516.MOV` uses 62% of the half-frame rounding budget; whether F2 is worth
   any work at all depends on this measurement and on nothing else.
2. **A genuine sub-frame VFR fixture.** Added to the generator as
   `h264_1080p_vfr_subframe` (240 fps source, irregular selection, so the jitter
   survives the encode where `setpts` jitter did not) but **not yet built or
   validated** — it was added after the matrix had run.
3. **An orientation marker in the burn-in plate**, if the rotation case is ever
   to be read from pixels rather than argued from byte-identical streams (3.5).
   A corpus regeneration; worth it only if rotation becomes suspect.
4. **An index-damaged MP4**, for F5, which has no test at all.

Items 1 and 2 are what decide whether F2 is urgent, merely true, or closeable.

### Step 2 — the HUD field for F6 (hours, not days)

Put the planar stand-down on the HUD. It is one field, it follows this codebase's
own rule that a silently-degrading configuration must be readable off the running
build, and it stops every future LUT-loaded measurement being taken on an
unlabelled path. Do it before anything that measures, not after.

### Step 3 — F7's two small items

The `scrubWalkCacheBudgetMs` initialiser and the Go to Frame clamp. Both are
contained, both are already understood, and the fill budget wants a sweep at the
same time since it has never been re-measured since the landing moved to the
worker.

### Step 4 — establish whether hardware decode is even available (F1, investigation only)

Before any design work: does the **shipped** minimal LGPL FFmpeg have D3D11VA
compiled in? Checked against the shipped DLLs, not the gyan build. If it does
not, F1 grows a toolchain component and the vcpkg pin constraint applies, which
changes its cost by an order of magnitude and therefore its position in this
list. **This is a one-hour question with a large consequence, which is why it is
ahead of the thing it scopes.**

### Step 5 — then choose between F1 and F2, and do not do both at once

They touch the same frame path from opposite ends, and each would invalidate the
other's measurements.

* **F2 (the index model) if step 1 shows real material losing frames.** It is a
  correctness problem, it is invisible to every counter Trace has, and
  correctness outranks speed on a review tool.
* **F1 (hardware decode) if step 1 shows the index model holds on real files.**
  Then the remaining inconsistency is per-frame cost, and F1 is the only thing
  that changes it by the factor needed — F4's pipeline work is worth ~10% on
  measured evidence and hardware decode is worth multiples.

**F4 is explicitly last**, and conditional: if F1 lands, a hardware-decoded frame
needs no conversion stage, and most of the two-stage design's reason goes with
it. Building F4 first would be building the stage that F1 deletes.

> **2026-10-31: that conditional resolved, and it resolved the other way.** F1
> did not land — it is built, measured and recommended off — so **F4 is no
> longer blocked by it**, and the stage F1 would have deleted is still there.
> Note what the F1 measurement says about F4 before anyone picks it up: on 4K
> H.264 at 60fps the whole handler is **2.0ms of a 16.67ms budget**, so there is
> nothing on the long-GOP class for a pipeline to buy either. F4's case rests
> entirely on the ProRes files, which is where CLAUDE.md's checkpoint-2 record
> already left it — +10% measured against +22% predicted, and stage two not
> justified by the measured margin.


---

## 7. What this session did not measure

Stated explicitly so none of it reads as cleared:

1. **Everything display-dependent.** Cadence, dropped and repeated frames, A/V
   offset, drag latency, pointer-to-picture, smoothness. The panel was at 59 Hz
   behind a 59.999 Hz Parsec virtual display; see 0.2. Re-run with the panel at
   5120x1440 @ 239.999 Hz and Parsec closed.
2. **Open time.** Measurable here, but worth taking in the same pass as the
   pacing work and against mpv, which is not installed.
3. **mpv as a reference** for anything.
4. **Feel.** The owner's hands on the real panel remain the final word, per the
   brief, and nothing in this document substitutes for that.
5. **Real-world files in the accuracy matrix.** They have no burn-in. Their
   properties are modelled by the synthetic rows; that is not the same as testing
   them.
6. **Open GOP as distinct from closed.** Requested in the fixture, not confirmed
   by the probe (2.3).
