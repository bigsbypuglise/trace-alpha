# MP4 consistency, phase 2 — real footage, and what it found

**Session of 2026-10-29. Branch `mp4-phase2`, cut from `mp4-review` at `f59b500`.**

The review (`docs/mp4-playback-review.md`) proved landing exact on eighteen
synthetic files and named three places the corpus had not reached: real VFR
phone footage (F2), per-frame decode cost (F1), and the silent LUT path (F6).
This session measured the first and found something else there.

Read section 0 before quoting any number.

---

## 0. Validity

### 0.1 THE DISPLAY WAS WRONG IN BOTH WAYS AGAIN, SO NO PACING FIGURE WAS TAKEN

Checked first, per the brief, and it failed both halves:

| | reported |
|---|---|
| active display path (`refresh.ps1`) | **1920x1200 @ 60.000 Hz** — a Parsec virtual display |
| the 4090's own panel (`Win32_VideoController`) | **1024x768 @ 60 Hz** — not 5120x1440 @ 239.999 |
| `parsecd` | **running**, two processes |

So, exactly as in the review: **no cadence, drop/repeat, drag-latency,
pointer-to-picture or smoothness figure was taken, and none is estimated
anywhere in this document.** The scrub paint gate's period is
`max(refresh, 2 x paint cost)` — ~4.4 ms at 240 Hz against ~17.5 ms at 60 Hz —
so a latency figure taken here would be measuring a different mechanism from the
one that ships.

**What this session measured instead is display-independent**, and that is not a
consolation prize: frame identity is read from the pixels of the frame, and the
burn-in says what it says whatever the panel is doing.

The FFmpeg 7.x DLLs (`avcodec-61`, `avformat-61`, `avutil-59`, `swscale-8`,
`swresample-5`) were deleted from `build/app/Release`, as the brief asked.

**THEY ARE NOT STALE LEFTOVERS, AND THE REVIEW AND THE BRIEF BOTH CALLED THEM
THAT.** The CI log settles it: `windeployqt` prints `Updating avcodec-61.dll`,
`Updating avutil-59.dll`, `Updating swresample-5.dll`, `Updating swscale-8.dll`
on a clean runner that has never held an old build. **They are Qt Multimedia's
own FFmpeg**, deployed beside Trace's every time the app is deployed. So:

* deleting them locally is **safe** — Trace's import table names `avcodec-63`,
  and its audio goes through `QAudioSink` on WASAPI with Trace's own FFmpeg for
  decoding. Verified after the deletion rather than assumed: a clip with sound
  plays with the HUD reading `audio aac 48000Hz 2ch … MASTER … proc 5126ms …
  clk 5.026s`, `under 0`, `skip 0`;
* they **come back** on the next `windeployqt`, and CI ships them in every
  package. A future session that deletes them again and expects them to stay
  gone will be surprised, and one that reads their presence as evidence of a
  stale build will be wrong.

The directory listing was never the trap. Believing a directory listing tells
you which DLLs a binary loads is the trap, and it points both ways.

### 0.2 Hardware decode: the review's step 4 is closed, and the answer is yes

Confirmed against the shipped DLL rather than the build script, with a control
search run first to prove the probe can find anything at all:

```
--enable-d3d11va  --enable-d3d12va  --enable-dxva2
h264_d3d11va h264_d3d11va2 h264_dxva2 h264_d3d12va   PRESENT
hevc_d3d11va hevc_d3d11va2 hevc_dxva2 hevc_d3d12va   PRESENT
av1_d3d11va  vp9_d3d11va                             PRESENT
h264_cuvid   hevc_cuvid   h264_nvdec                 absent
```

So F1 needs no toolchain change. NVDEC is absent and does not matter: D3D11VA is
the Windows route on NVIDIA too.

**Hardware decode was nevertheless NOT started this session** — see section 4.

---

## 1. A burn-in of real footage, and proving it is still the same footage

`scripts/measure/make_iphone_burnin.ps1` re-encodes the pool's two iPhone clips
with the corpus plate over them. Written to
`Trace_Testing_Assets/24_iPhone_BurnIn/`, three copies:

| copy | rotation | purpose |
|---|---|---|
| `IMG_0516_burnin_norot.MOV` | stripped at the input | the measurement copy, 511 frames |
| `IMG_0563_burnin.MOV` | none in the source | the measurement copy, 382 frames |
| `IMG_0516_burnin_rot.MOV` | **-90 preserved** | the rotated path, not machine-readable |

**The timestamps survive, and that is asserted rather than asked for.** The
review already paid for this lesson once — its `h264_1080p_vfr` fixture asked
`setpts` for sub-frame jitter and the encoder's time base quantised it away, so
the file tested something other than its own filename. The generator therefore
reads the output's whole PTS list back and compares it with the input's element
by element. All three copies: **time base `1/600` preserved, frame count
preserved, and every one of 511 / 382 / 511 pts ticks identical.**

**`-enc_time_base` is load-bearing and was proven so before being trusted.** On
a 74-frame probe of IMG_0516, with it the output pts list is identical; with it
omitted and every other term unchanged — `-fps_mode passthrough` still set, the
muxer timescale still 600 — **43 of 74 ticks differ.** The first dozen match
either way, so a spot check of the head of the file passes on the broken
configuration. (`-enc_time_base -1`, the "copy the input" spelling, is rejected
outright by FFmpeg 9: *"Invalid time base: -1"*.)

**The copies carry the same timing character as the originals**, which is the
property that makes them a model of the real thing:

| file | frames | maxoff | jitter | GOP (max/mean) |
|---|---|---|---|---|
| `IMG_0516.MOV` original | 511 | 0.310 | 0.0396 | 24 / 20.0 |
| `IMG_0516_burnin_norot` | 511 | **0.310** | **0.0396** | 24 / 23.9 |
| `IMG_0563.MOV` original | 382 | 0.040 | 0.0399 | 20 / 19.8 |
| `IMG_0563_burnin` | 382 | **0.040** | **0.0399** | 24 / 23.8 |

The GOP is the one thing that is close rather than identical: libx265 at
`keyint=24` is regular where the camera also inserts scene-cut keyframes, so the
copies carry slightly fewer keyframes over the same maximum interval. Stated
because it is the only property of these files that is not the original's.

The sub-frame VFR fixture the review added and never built is built — see
section 5.

---

## 2. THE READER WAS WRONG BEFORE TRACE WAS, AND IT ACCUSED A CORRECT BUILD

The first run of the matrix on real footage looked like a disaster: ten
mismatches on IMG_0516 with a worst delta of 247, including `goto 383 -> 256` and
`step+ 259 -> 12`. Reproduced identically at a 4.4x longer settle, so not a race.

**It was the instrument.** `readburnin.py` locates the plate by scanning
rightward from the left edge until the grey run breaks, which silently assumes
the picture beside the plate is not itself neutral grey. Synthetic `testsrc2`
satisfies that. **A street scene panning onto asphalt does not.** The run
continued past the plate's true right edge into the road surface, the box came
out too wide, the ten cell centres were spread across it, and the reader returned
confident wrong numbers.

**It was settled by pulling the frames out of the file with ffmpeg and looking at
them.** Frame 510's plate shows the human-readable `F 510` and eight lit cells —
`0111111110`, which is 510, drawn correctly — and the reader called it 504.
Frame 383's shows `F 383` and `0101111111`, and the reader called it 382. In both
captures the asphalt sits flush against the plate's right edge.

**The old aspect guard could not catch it**: widening 2220 → ~2900 px takes the
aspect from 0.117 to ~0.090, which is inside the 0.6–1.6x tolerance that same
guard needs in order to survive scaling.

**The fix is to stop measuring the one thing the picture cannot be trusted
about.** The plate's right edge is now DERIVED from the measured height and the
design's own 1110:130 ratio; the height is reliable where the width is not,
because the row test demands >90% plate-like across the *left half of the
picture* and content has to work much harder to satisfy that than a single
column does. The scan is kept as the **occlusion guard** — the one thing it is
good at, since no amount of grey content can make a run stop early. A per-cell
**uniformity** test is added as the backstop the aspect check used to be: a
correctly located cell is a solid fill, a mislocated one is a mixture, and that
is the only test that sees where the samples actually landed.

**Validated exhaustively, with Trace absent** (`scripts/measure/checkburnin.py`
decodes a clip frame by frame and compares each read against the index ffmpeg
produced it at, at the downscale a window capture gives):

| clip | frames | mismatch | unreadable |
|---|---|---|---|
| `IMG_0516_burnin_norot` | 511 | **0** | 0 |
| `IMG_0563_burnin` | 382 | **0** | 0 |
| `h264_1080p_24_gop48_b3` (corpus regression) | 480 | **0** | 0 |
| `h264_1080p_vfr_subframe` | 500 | **0** | 0 |

**1873 frames, nothing wrong.** Proven able to fail, ten controls: no plate →
refused; a doctored MSB → **525** (= 13 + 512) exactly as predicted; the plate's
right-hand end occluded → refused; contaminated height at three severities and
from both directions → all refused by the new guards; a half-filled cell →
caught by the uniformity test; and **grey painted flush against the plate's
right edge — the real case — now read correctly.**

**The lesson generalises and is the opposite of the one the file already
carried.** That file's own header says the cell centres are derived from the
plate it *measured*, because "a formula cannot notice it has drifted". True, and
incomplete: a measurement cannot notice it has been contaminated either. Measure
what the picture can tell you reliably, derive what it cannot, and turn the thing
you stop measuring into a guard rather than discarding it.

---

## 3. THE FINDING: an open-GOP seek lands past its target and renames the frame

With the reader fixed, the real-footage matrix came back with a small, stubborn
residue: `goto 286 -> 288`, `goto 191 -> 192`, `goto 383 -> 384`. One and two
frames, not hundreds.

### 3.1 It is not F2. The index model is exact on this footage.

F2 is the review's finding that Trace maps an index to a timestamp by a nominal
rate, so "frame N" means "the frame nearest N/fps" rather than "the Nth frame".
Applying exactly that arithmetic to these files' real timestamps:

```
IMG_0516_burnin_norot   ordinal != pts-index on   0 of 511 frames
IMG_0563_burnin         ordinal != pts-index on   0 of 382 frames
```

under **either** candidate rate (`r_frame_rate` and `avg_frame_rate`, which
differ on both files). So on genuinely variable-rate phone footage that uses 62%
of the half-frame rounding budget, **the nominal-rate index model is exact
everywhere and F2 costs nothing.** That closes the question the burn-in was built
to answer, in Trace's favour.

### 3.2 What it is: open-GOP leading pictures, and a clamp that hid them

Both clips are **open-GOP HEVC** — 15 of 16 and 21 of 22 keyframes carry
*leading pictures*, frames that follow the keyframe in decode order and precede
it in presentation order. Read from the container, by looking for packets after
a keyframe whose PTS is earlier than the keyframe's.

Every bad landing is exactly a keyframe's presentation index: **288, 192, 384.**

And it only happens when the move requires a **seek**. Approached by single
steps, 284, 285, 286, 287, 288 all land exactly, twice over. Alternated with
frame 0 so every target is reached by a jump, 286, 287 and 191 fail while 285 and
288 pass — reproducibly.

**Confirmed in a single clean run, and the HUD is the damning part.** Go to Frame
286 from frame 0 on `IMG_0563_burnin`:

```
seek log : seek#2 reason=StepJump requested=286 current=0 codec=hevc
HUD      : F:286 ... scrub exact | target 286 | shown 286 | delta 0 | walk 0f
picture  : burn-in reads 288
```

**Trace displayed frame 288, labelled it 286, and reported `delta 0`.**

The mechanism, in one line of `VideoDecoderFFmpeg.cpp`:

```cpp
// A backward seek should never land past the target; if a
// broken index puts us there, clamp so the UI stays put.
if (decodedFrame > impl_->seekTargetFrame) decodedFrame = impl_->seekTargetFrame;
```

A seek to a frame that is a leading picture of the next keyframe lands on the
keyframe; its leading pictures reference frames before it and cannot be decoded,
so they are discarded and the keyframe is the first frame out. Its index is
*past* the target, the clamp rewrote it to the target, `walk 0f` because the stop
test was satisfied immediately, and the frame was delivered under a number that
was not its own.

**This is the July 2026 scrub bug's exact shape** — `scrub exact | delta 0` while
displaying a frame most of a GOP away — and it survived for the same reason that
one did: **the telemetry asserted its own correctness instead of measuring it.**
It is also precisely what the burn-in instrument exists to find, and it is the
first thing it has found that the decoder's own accounting could not.

### 3.3 The fix: seek further back, never rename

The clamp is replaced by a fourth walk outcome, `Overshot`, returned to the
caller — because the seek is the caller's, and retrying inside a function whose
contract is "decode forward from where you are" would hide a second seek in it.
The caller re-seeks with a growing backoff (8, 32, 128, 512 frames) while still
aiming at the original target, and the walk forward from the earlier keyframe is
the ordinary exact-landing mechanism doing its ordinary job. Landing earlier is
always safe; it costs the frames between, and only on the landings that were
wrong before.

**The schedule starts at 8, and the first version starting at 1 was measured
wasteful rather than reasoned into shape.** 1 looks right — the landing is only
a frame or two past the target — and it *is* right for a Go to Frame, which
resolved at the first attempt. It resolved nothing on continuous reverse
playback, where every overshoot needed two attempts:

```
overshoot requested=359 landedOn=360 attempt=0 backoff=1
overshoot requested=359 landedOn=360 attempt=1 backoff=8
overshoot requested=335 landedOn=336 attempt=0 backoff=1
overshoot requested=335 landedOn=336 attempt=1 backoff=8    ... every 24
```

29 re-seeks against 21 real seeks — three seeks where one would do, on the one
playback mode that is already seek-bound. **The reason is geometric and is why 1
cannot work in general:** the seek landed on that keyframe *because* its index
entry is at or before the target, so asking for `target - 1` is still at or
after it and finds the same keyframe. Clearing it means clearing the keyframe's
leading-picture run — 3 frames here, a handful anywhere. 8 clears it with margin
while staying well inside a GOP; 32 then clears a typical GOP and 128 a long one.

Re-measured on the same gesture: **re-seeks 29 → 15, and every overshoot now
resolves on the first attempt**, `unresolved` still 0. **Found only because the
seek log records the attempt number and the backoff** rather than just counting
re-seeks — a bare counter would have read "29 re-seeks, all resolved" and looked
like success.

**The last attempt delivers the landed frame under its OWN index**, so a file
that defeats all four backoffs shows a non-zero `delta` instead of a quiet lie. A
review tool may show the wrong frame when the file beats it. It may not call it
the right one.

Two counters, `overshoot <reseeks>/<unresolved>`, on the HUD beside `recov` and
in the `--scrub-selftest` report. The second must stay 0.

**Measured, same gesture, same settle, before and after:**

| leg | before | after |
|---|---|---|
| `goto 286` (from 0) | **288** | **286** |
| `goto 287` (from 0) | **288** | **287** |
| `goto 191` (from 0) | **192** | **191** |
| `goto 285`, `goto 288` (controls) | 285, 288 | 285, 288 |

---

## 4. THE DECISION: hardware decode did not start, and the rule is the brief's own

The brief's rule: *"if real footage loses or mislabels frames, F2 is this
phase's main work and hardware decode does not start this session."*

Real footage **mislabelled** frames. The cause was not F2's index model — which
measured exact — but a seek-landing clamp, and the rule's trigger fired either
way. So hardware decode was not started, and the session's main work became
finding and fixing what the real footage actually exposed.

That is the better trade on this project's own ordering: a correctness fault
invisible to every counter Trace has outranks a speed one that every counter can
see. The groundwork that is cheap and useful regardless did land — the GPU
adapter name and the decode path are now in the issue report (section 5) — and
the decode path reads `software`, honestly, because that is what this build is.

**What hardware decode still needs, unchanged and unstarted:** the per-codec and
per-pixel-format gate, per-file fallback with a logged reason, the HUD field, the
copy-back first cut, and the colour gate against software output. None of it is
blocked by anything here.

---

## 5. The smaller items

### 5.1 F6 — the planar stand-down is on the HUD

`syncPlanarOutput` stands the GATE C planar upload down whenever a colour
transform is active, because the stage works on BGRA. Nothing on screen said so,
so a reviewer with a LUT loaded was on a measurably different playback path from
every figure recorded in this repo and could not tell.

The HUD's line 0 now carries **`planar on` / `off (color)` / `off (env)` /
`off (renderer)`**, beside `renderer`. The **state** is read back off the
decoder and the **reason** off the function that decided it, so a stand-down
that failed to reach the decoder reads as `on (color)` — a contradiction on its
face — rather than as a confident `off`. Same rule as `renderer`, `font` and
`strip`, and the one state that matters is reached from the View menu rather
than from a launcher, so it cannot be inferred from how the build was started.

### 5.2 F7 — the fill budget has one number, and Go to Frame refuses

**`scrubWalkCacheBudgetMs`** had a member initialiser of `240.0` that `open()`
overwrote unconditionally with `envInt("TRACE_SCRUB_FILL_MS", 60)`. 240 was dead
code and 60 shipped, under a comment arguing for 240 off a sweep table. There is
one named constant now, used by both, so changing the shipping behaviour means
changing that line. The value is settled in section 6.

**Go to Frame** clamped out-of-range input silently: on a 480-frame clip, typing
`900` left `90` in the field — the `QSpinBox` validator swallowed the third
keystroke — and OK landed the playhead on frame 90. The spin box's range is now
wider than the clip and the **refusal is Trace's**, in the same shape as Go to
Timecode's, which had the rule right all along. A range the user cannot leave
cannot report that they tried to.

### 5.3 The issue report carries the GPU and the decode path

`buildIdentity()` — About, and the Report an Issue mail body — gains
`GPU: <adapter>` and `Decode: <path>`. Trace is on several machines now, and
those are the two facts every report about smoothness or a wrong picture turns
on, neither of which a tester can be asked to look up and both of which are
decided at runtime. The adapter comes from the `IDXGIAdapter` the D3D11 backend
already holds, asked of the **adopted** renderer, and the line is omitted
entirely rather than printed empty on the CPU path.

### 5.4 `h264_1080p_vfr_subframe` is built, and does not test what its name says

Built and validated. It carries genuine sub-frame timing — durations of 8, 10
and 12 ticks at a 1/240 time base — but its **`r_frame_rate` comes out
`120/1`**, because ffmpeg derives that from the greatest common divisor of the
frame durations and `gcd(8,10,12) = 2`.

**That is structural, not a mistake in this fixture.** `r_frame_rate` is
`timescale / gcd(durations)`; for it to equal the nominal rate, every duration
must be a multiple of a frame period — which is exactly what "sub-frame jitter"
rules out. **A synthetic file cannot have both.** The iPhone files escape it
because 506 of their 511 durations are the same value and ffmpeg's rate
estimator latches onto it, landing on `24000/1001`.

So the fixture tests *an inflated nominal rate*, which is a real shape worth
having — and the case it was built for is now covered by real footage, which is
better. Modelled at 24 fps it predicts 40 collisions; at its own 120 it predicts
1897 empty indices. Its matrix result is in section 6.

---

## 6. Results

### 6.1 Real footage, 17 legs each, burn-in read from the picture

| clip | frames | legs | mismatch | unreadable | worst delta | verdict |
|---|---|---|---|---|---|---|
| `IMG_0516_burnin_norot` | 511 | 17 | **0** | 0 | **0** | **PASS** |
| `IMG_0563_burnin` | 382 | 17 | **0** | 0 | **0** | **PASS** |
| `IMG_0516_burnin_rot` | 511 | 17 | — | 17 | — | **not readable by this harness** |

Before the fix, on the same gesture set with the same fixed reader: IMG_0516
**1 mismatch**, IMG_0563 **7 mismatches**. Both are 0 now.

**The rotated copy is unreadable and that is the reader working.** Its plate
comes out rotated on screen, and the bit strip is MSB-first left to right, so a
rotated capture cannot be read — and a 180-degree one reads as a *valid but
wrong* number, which is the one outcome the instrument exists to prevent. The
new guards refuse it in three different ways (`bit cell 0 is not a solid fill`,
`no plate row found`, `plate derived as 0..1136 but the image is only 444px
wide`) rather than returning a number. The rotation case is answered by the
review's stronger argument instead: `rotationDegrees` is applied in the renderer
and is read by neither `frameFromPts` nor `seekToFrame`.

---

### 6.2 The synthetic corpus — the no-regression gate, and a second finding

One command, on the shipping binary: **`FILES 19 | PASS 17 | FAIL 2`**, and both
failures are accounted for.

| file | result | account |
|---|---|---|
| the other 17, including `h264_1080p_editlist` | **PASS**, 17 legs, worst delta 0 | the edit-list fixture now passes in the plain run because its 48-frame offset is declared in the harness (section 6.7). It is trimmed 48 frames into an already-burned source, so `displayed == requested + 48` is correct edit-list handling. |
| `h264_1080p_rot90` | **unreadable, 17 legs** | by design: the bit strip is MSB-first left-to-right and the reader refuses a rotated capture rather than returning a confidently reversed number. Answered by the review's byte-identical-stream argument instead. |
| `h264_1080p_vfr_subframe` | **13 mismatches, worst delta 395** | new fixture, never run before, and it fails identically before and after the seek change. See below. |

**So there is no regression on the eighteen files the review measured**, which is
the gate this had to clear. Run twice: once on the binary before the backoff
correction and once after, with the same result on every row.

#### The nineteenth file is new, and it fails — demonstrating F3

`h264_1080p_vfr_subframe` had never been run. It fails 13 of 17 legs with a worst
delta of **395**, and the cause is the review's **F3**, which that document
demoted to "a real hazard in principle" for want of a file that showed it:

> `metadata_.frameCount` is `stream->nb_frames` … `maxFrame()` returns
> `frameCount - 1` … **Nothing checks that the index arithmetic can actually
> reach that value.**

This file's `r_frame_rate` is **120/1** against a real rate near 24 (section
5.4), so Trace's index arithmetic spans **0..2396** while `maxFrame` is
**499** — the two numbers are computed by unrelated means and here they disagree
by a factor of five. Predicted from the container before the run and confirmed
by it: `goto 498` should display ordinal **104**, and the measured worst delta of
395 is `498 - 103`.

**This is not a regression and not the open-GOP bug** — the fixture is new, and
the failure reproduces the arithmetic exactly. It is recorded rather than fixed:
F3's fix is the same container-frame-table rework as F2's, which the review
judged not obviously worth doing, and nothing in this session changes that
judgement. **What has changed is that F3 is now demonstrated rather than
theoretical**, on a file shape that is not purely synthetic — irregular frame
durations with a small greatest common divisor are what screen-capture and some
phone and AI export pipelines produce, and that is exactly what inflates
`r_frame_rate`.

### 6.3 The fill-budget sweep — flat, so 60 stays

`scripts/measure/fillsweep.ps1`, 4K H.264, through `--scrub-selftest` so it
resolves no window. Budgets 30 / 60 (the shipping default, run with the knob
*unset* rather than set to its own value) / 120 / 240, two reps each.

**Leg 2, the reversal drag — the gesture this budget exists for:**

| budget | dec f/s | seeks | rev-hit | ins | walk max | hitch | delta |
|---|---|---|---|---|---|---|---|
| default (60) | 167.6 / 167.0 | 4 | 97.9% | 267 / 265 | 11f | 3 | **0** |
| 30 | 166.9 / 166.2 | 4 | 97.9 / 97.8% | 267 / 265 | 11f | 3 | **0** |
| 120 | 166.3 / 166.1 | 4 | 97.8 / 97.9% | 267 / 265 | 11f | 3 | **0** |
| 240 | 167.0 / 167.1 | 4 | 97.9% | 266 / 264 | 11f | 3 | **0** |

**Nothing moves.** Not the seek count, not the hit rate, not the insert count,
not the decode rate. The forward leg is flat too and reads `rev-hit 0.0%` at
every budget, which is the control working: a forward drag never leaves the
walked run, so the fill has nothing to buy there.

So **60 stays, measured rather than assumed**, and the comment in the source now
says so. A plausible reason the recorded 240 ms win stopped reproducing is the
async scrub batch of 2026-08-14 — one request now covers up to four consecutive
frames, so the cache is filled by the batch walking frames it was going to
decode anyway — but that is an explanation offered for a flat result, not a
second measurement, and it is labelled as one in the source.

**`delta 0` on all 16 rows**, so no budget changes the landing. The latency
columns from the same runs are deliberately not quoted (section 0.1).

### 6.4 Regression, and the proof that the new counter can fire

**`scrubbar.ps1` full pool: PASS — 22 files, 88 legs, `delta 0` throughout**, on
the shipping binary with the fix in.

**And the fix is provably inert there: `overshoot 0/0` on all 88 legs.** It never
engages on the existing pool, so nothing previously measured changes — which is
what you want from a fix whose whole job is to catch a case the pool does not
contain.

**That zero is a real zero, not an empty measurement**, because the counter was
proven able to fire first — the rule this project has for every new instrument.
On the iPhone clip, with `TRACE_SEEK_LOG=1`:

```
overshoot req#3 requested=286 landedOn=288 attempt=0 backoff=1 accept=0
```

One re-seek, at the first backoff, and the picture then reads **286**. The HUD
reads `walk 22f` where it read `walk 0f` before — the walk forward from the
earlier keyframe is visibly doing the work.

Selftests, all green on the shipping binary: `renderer=d3d11 fellback=0
planar=1` · `renderer=cpu fellback=0 planar=0` · `OK - 11 shapes x 4 scale
factors` · `OK - 14 channel layouts` · OCIO `version=2.5.2 … moved=1` ·
`verify_trace_assets --strict` at **33 embedded files**.

*(`--renderer-selftest=cpu` ASSERTS the adopted backend, it does not select it;
`TRACE_RENDERER=cpu` is what selects. Invoked without the env it correctly fails
with exit 4 and "expected renderer 'cpu' but 'd3d11' is presenting" — the check
working, not the build.)*

### 6.5 What the fix costs on the scrub path, A/B'd on one binary

`--scrub-selftest` on the open-GOP iPhone clip, with the knob and without it —
which is what the knob is for, since the fault is file-dependent and the pool
does not contain it.

| leg | | fix on | fix off |
|---|---|---|---|
| 1 forward sweep | overshoot / delta | 0/0 · **0** | off/0 · **0** |
| 2 reversal drag | overshoot / delta | **1**/0 · **0** | off/0 · **0** |
| | seeks · rev-hit | 9 · 90.7% | 9 · 89.4% |
| 3 slow forward | overshoot / delta | 0/0 · **0** | off/0 · **0** |
| 4 rapid back-and-forth | overshoot / delta | **2**/0 · **0** | off/0 · **0** |
| | seeks · rev-hit · ra-walk · walk max | 11 · 78.4% · 9.20f · 19f | 11 · 74.3% · 1.71f · 8f |

**All four legs PASS with `delta 0` in both configurations**, so the drag path's
exactness is unchanged either way — the release landing is a separate exact Step
and was always correct.

**The overshoot fires three times across four legs and resolves every time**
(`unresolved 0`). It is rare even on the material that produces it.

**Its cost is visible and is the mechanism, not a surprise:** on the rapid leg
`ra-walk` goes 1.71 → 9.20 frames per seek and `walk max` 8 → 19, because
instead of accepting the keyframe it landed on, the decoder seeks back and walks
forward. That is the fix doing exactly what it is for. `rev-hit` is *higher*
with it on (78.4% against 74.3%, and 90.7% against 89.4%), so the cache is not
being hurt.

**`overshoot off/0` rather than `0/0` when the knob is down** — the field
refusing to let "nothing overshot" and "the thing that would have noticed is
switched off" read alike, working as designed.

**Release latency differed between the two columns and is deliberately not
quoted** (section 0.1). It is the one term here that is display-dependent, and
it is the thing to look at first if this fix is ever suspected of costing scrub
feel on a machine in its recorded mode.

### 6.6 The iPhone originals open, and play the right audio track

Both, with `TRACE_OPEN_LOG=1`. They carry eight streams each — video, stereo
AAC, a 4-channel `apple_apac` spatial track, and five `mebx` metadata tracks
ffprobe cannot identify — and Trace reads all eight without complaint:

```
IMG_0516.MOV  streams=8 audioIdx=1 audio=aac/48000Hz/stereo/2
              fpsQ=24000/1001 tb=1/600 rot=90 dar=0.562500 frames=511  openMs=12.35
IMG_0563.MOV  streams=8 audioIdx=1 audio=aac/48000Hz/stereo/2
              fpsQ=24/1      tb=1/600 rot=0  dar=1.777778 frames=382  openMs=12.63
```

**`audioIdx=1` is the right track**, and not by luck: `av_find_best_stream`
prefers a stream with `disposition:default`, and the AAC track carries it while
the `apple_apac` one does not — checked in the container rather than inferred
from the result. The spatial track is higher-bitrate and 4-channel, so a
bitrate-or-channel-count heuristic would have chosen it.

`rot=90` on IMG_0516 is correct and is not a disagreement with the container's
`rotation=-90`: `av_display_rotation_get` reports counter-clockwise and Trace
stores clockwise.

### 6.7 Harness changes, and one trap removed

- **`make_iphone_burnin.ps1`** (new) — the burn-in copies, with the PTS
  assertion.
- **`checkburnin.py`** (new) — validates the reader against ground truth with
  Trace absent. This is the tool that separates a reader fault from a player
  fault, and the session needed exactly that separation.
- **`readburnin.py`** — the derived right edge, the occlusion guard and the
  per-cell uniformity test (section 2).
- **`fillsweep.ps1`** (new) — the fill-budget sweep, driving `--scrub-selftest`
  so it resolves no window.
- **`mp4matrix.ps1`** — **per-clip burn-in offsets, declared in the harness.**
  `h264_1080p_editlist` is trimmed 48 frames into an already-burned source, so
  `displayed == requested + 48` is correct edit-list handling. The offset used
  to be the caller's to remember, so a plain full-corpus run reported 17
  mismatches at a dead-constant delta of 48 — a by-design result that reads
  exactly like a regression. It is declared once, in the script, and still
  declared rather than inferred: a harness that worked its own offset out from
  the first frame would absorb a genuine 48-frame landing bug into it.

---

## 7. What this session did not measure

1. **Everything display-dependent** — cadence, drops, repeats, A/V offset, drag
   latency, pointer-to-picture, smoothness. Section 0.1.
2. **Hardware decode**, at all. Section 4.
3. **Feel.** The owner's hands on the real panel remain the final word.
4. **The rotated copy's frame accuracy from pixels.** The bit strip is MSB-first
   left-to-right, so a rotated capture cannot be read and the reader refuses it
   rather than returning a confidently reversed number. The copy exists for the
   rotated path, for opening and for the eye.
5. **mpv** as a reference, and **open time**. Both carried from the review.
6. **The accelerated SHUTTLE ladder on open-GOP material.** The scrub and drag
   gestures are covered (section 6.5: three re-seeks across four legs, all
   resolved, exactness unchanged) and so is continuous reverse at 1x and the
   first rungs (section 3.3: 15 re-seeks over a ~14s run, one per GOP boundary,
   each resolving at the first attempt). What is *not* covered is the 5x/10x/30x
   ladder, where the stride means most targets are not adjacent to the last one.
   The counter is on the HUD and in the selftest report, so it is visible if it
   bites.
7. **The release-latency cost of the overshoot re-seek.** Observed to differ
   between the two configurations on the rapid-throw leg and NOT quoted, because
   it is the one term in that comparison that depends on the display. First thing
   to measure on the panel.
8. **What the reverse-playback re-seeks cost in TIME.** Section 3.3 measures
   their count (15 over a ~14s reverse run, one per GOP boundary, each resolving
   at the first attempt) and that count is display-independent. What a seek plus
   a short walk costs against the reverse shuttle's own budget is a pacing
   figure, so it was not taken. `TRACE_SEEK_OVERSHOOT_RESEEK=0` is the A/B for
   it on one binary.
