# The image-sequence cadence instrument, and the first EXR playback figures

Record of what was built and measured. 2026-08-24, branch
`exr-stage0-dependencies`. **This is STEP 1 of stage 3 and it is stage 3's
CONTROL, not a side quest.** Stage 3 puts an OCIO `DisplayViewTransform` on the
EXR path; whether that costs anything cannot be judged without a before-figure,
and until today this project had none.

**Nothing about the Color Transform dialog was built. Nothing about playback
behaviour was changed.**

---

## The display, checked before anything was measured

`scripts/measure/refresh.ps1` plus `Get-CimInstance Win32_VideoController`, both,
because the first describes the ACTIVE path and will happily report a virtual
display without saying the panel changed underneath it.

- Active path **5120x1440 @ 239999/1000 = 239.999000 Hz** -- the physical panel in
  the mode every recorded figure in this project was taken in.
- `parsecd` is running as a background service, but the **Parsec Virtual Display
  Adapter has no mode set** and the 4090's own panel is the active path. Not a
  remote session.

So the figures below are panel figures and are comparable with the record. (The
stage 2 part 1 document is explicit that none of ITS figures are, because that
session ran over Parsec with the panel additionally at 59Hz.)

---

## WHAT WAS ACTUALLY MISSING -- narrower than "the sequence path has no counters"

The handoff position was that the image-sequence path exposes no cadence counters
and that building them means giving `ImageSequenceFrameSource`'s tick the video
path's instruments. **Read before written, and most of it was already there.**

The sequence path is not a separate tick. It runs the SAME `playTimer_` handler,
the SAME GATE E absolute-deadline scheduler (`armNextPresent()` is called from a
scope guard on every exit path regardless of media kind), and the SAME
`notePresentedPlaybackFrame()`. So `presented`, the cadence-gap histogram,
`handler>budget`, `tick-late`, `tick-stall`, `period` and `rephase` were all being
**accumulated for image sequences already**.

What was missing was three things, and only the first is large:

1. **Display.** `refreshHud()` builds the three cadence lines inside
   `if (currentMedia_->kind == MediaKind::VideoFile)` and nowhere else. Every one
   of those counters was measured and never shown.
2. **Two measurement gates.** Tick jitter and present-late were gated on
   `isVideo`, so they alone were genuinely not accumulated.
3. **One counter that did not exist at all** -- see `skip`, below.

**This is the same failure `stalls 0 of 0` produced on the title-bar stall**: an
instrument with no samples reads exactly like an instrument reporting health, and
it was read as a clean result across a dozen harness runs. Here it produced a
recorded claim that is now measurably wrong -- see "the premise that expired".

---

## What was built

**One instrument for every timed media kind, extracted rather than copied.**
`MainWindow::cadenceHudLines(rateFps, rateIsNominal)` is the three lines, called
from the video branch and from the image-sequence branch. A second copy for
sequences would be a second instrument to keep in agreement, and this file
already records twice what that costs (`notePresentedPlaybackFrame()` and
`beginPlaybackTimeline()` were both extracted for exactly that reason).

**`notePresentLatency()`** likewise, for the same reason. It is CALLED from the
two branches rather than hoisted above them, because the video branch's call site
sits after an early return (the control path's accumulator gate) that the
sequence branch does not have -- and moving that return is a BEHAVIOUR change
this instrument must not make.

**`measureCadence` is a deliberately separate expression from `isVideo`.** The
first is a measurement gate, the second a behaviour gate. `isVideo` still decides
the accumulator gate and the real-time drop and is untouched. Keeping them
separate is what makes the baseline a measurement of the sequence path **as it
already is** rather than as this change made it.

**`skip` -- the counter that did not exist.** On the sequence branch `steps` is
`floor(accumulator / period)` with a floor of 1, so a frame that misses its budget
banks the shortfall and the next tick's target is two or more frames on. Those
frames are never loaded and never presented. That is the same visible outcome the
video path calls `drop`, reached by a different mechanism: the video path drops
DELIBERATELY through `realtimeDropSteps()` and holds media time on the clock,
while this is the shared accumulator catching up on its own.

**It is named `skip`, not `drop`, and that is not fussiness.**
`realtimeDropSteps()` is never called on this branch. Printing both as `drop`
would claim the owner's 2026-08-13 real-time-drop policy is running on a path
where it is not -- the class of dishonesty the Movie Inspector's origin tags and
the colour line's `(inferred)` marker exist to prevent. `media` means the same
thing on both.

**`fps nominal`, and that word is load-bearing.** An image sequence has no
container rate: `ImageSequenceFrameSource::fps()` returns the 24.0 Trace
synthesises and `fpsRational()` returns false. So the denominator of "% of real
time" is a Trace assumption, and printing it bare would claim a source rate the
file does not state -- spec phase 7's rule for timecode, applied to frame rate.

---

## THE BASELINE

`scripts/measure/seqcadence.ps1`, panel, shipping renderer (`d3d11 +overlay`),
`TRACE_HUD=1`, scratch settings file, colour transform OFF (`map Gamma 2.2`),
root pass in both cases.

### `6_Image_Sequence\EXR_SEQ\R2_OP_Stacks_01` -- 217 frames, 1920x1080, 3ch, PIZ

| | rep 1 | rep 2 |
|---|---|---|
| presented | **23.97 / 24.00 fps nominal (99.9% real time)** | **23.97 (99.9%)** |
| frames / elapsed | 216 / 9.01s | 216 / 9.01s |
| skip | **0** (media 99.9%) | **0** (media 99.9%) |
| handler>budget | **0 of 215** (max **37.4**ms) | **0 of 215** (max **36.6**ms) |
| cadence p50/p95/p99/max | 41.6 / 43.2 / 44.3 / 45.8 | 41.7 / 43.1 / 44.7 / 46.4 |
| buckets | `~1x 214`, `1.1-1.5x 1`, rest 0 | `<0.9x 1`, `~1x 213`, `1.1-1.5x 1` |
| jitter last/avg/max | -0.69 / 0.59 / 2.03 | 0.68 / 0.68 / 2.43 |
| rephase / tick-late / tick-stall | 0 / 0 of 215 / 0 | 0 / 0 of 215 / 0 |
| drift | -13.0ms | -12.0ms |

**This is a clean run by every standard the video path is held to.** It is
indistinguishable from a healthy ProRes run.

### `15_Redshift_ACES_EXR\MultlayerAces` -- 97 frames, 1920x1080, 27ch, DWAA (lossy)

| | cold 1st | rep 1 | rep 2 | rep 3 |
|---|---|---|---|---|
| presented | 5.78 (**24.1%**) | 6.90 (**28.8%**) | 6.97 (**29.0%**) | 7.01 (**29.2%**) |
| frames / elapsed | 24 / 4.15s | 28 / 4.06s | 29 / 4.16s | 29 / 4.14s |
| **skip** | **75** (media 99.4%) | **68** (media 98.6%) | **70** (media 99.1%) | **68** (media 97.7%) |
| handler>budget | 23 of 23 (max **203.1**) | 27 of 27 (max **191.1**) | 28 of 28 (max **189.7**) | 28 of 28 (max **182.7**) |
| cadence p50 / max | 156.0 / 514.2 | 145.6 / 189.8 | 152.5 / 188.1 | 147.7 / 184.8 |
| buckets | `>2.5x 23` of 23 | `>2.5x 25` of 27 | `>2.5x 24` of 28 | `>2.5x 25` of 28 |
| rephase | 23 | 26 | 27 | 28 |
| tick-late / tick-stall | 22 of 23 / 22 | 26 of 27 / 25 | 27 of 28 / 24 | 28 of 28 / 25 |
| drift | -3149ms | -2889ms | -2954ms | -2928ms |

**Read the two figures together and the sentence is:** this file plays 97 frames
of media in ~4.1 seconds -- i.e. on the clock -- **by showing 29 of them and
skipping 68.** `media 97.7-99.4%` and `real time 28.8-29.2%` are both true and
they are different questions. The first quoted alone is the mistake this
instrument exists to make impossible.

**The cold first run reads 24.1% and every warm run reads 28.8-29.2%.** Do not
read a cold first sweep as a result; that is the recorded `scrubbar.ps1` lesson
arriving in a new harness.

---

## THE PREMISE THAT EXPIRED, and it was written by this project two days ago

`docs/exr-stage2-float-buffer.md` records: *"Sequence playback is unchanged, and
honestly so: on the 27-channel DWAA sequence over a 2.5s window the new build
reaches frame 61 and 63 against the control's 65 and 63 ... Both keep real time
at 1080p, because prefetching hides the decode."*

**Reaching frame 61 in 2.5s is 24.4 index/s, and the playhead advances by
SKIPPING.** That figure is `media`, not `presented`. Measured today on the same
file: the frame index advances at ~23/s while pictures arrive at 6.9/s. The
observation was correct and the conclusion drawn from it -- "keeps real time" --
was a statement about the clock read as a statement about the picture.

That document was scrupulous about labelling: it says *"the sequence path exposes
no cadence counters, so no rate figure is claimed."* The claim leaked in anyway,
one sentence earlier. **Fourteenth premise expiry, and the second written by this
project's own recent session.**

---

## VIDEO IS UNMOVED -- measured against a control, not asserted

Control built from `2d09d89` in a worktree. **DLL payloads made byte-identical by
hash** (56 files, hashes concatenated and compared) so the executable is the only
variable, and **the two binaries proven distinct by their own strings** rather
than by a hash alone: a UTF-16 search of each finds `" fps nominal"` present in
the new binary and **absent** in the control.

| leg | new | control |
|---|---|---|
| 4K H.264 cadence x2 | **100.0 / 100.0%**, 120 frames, `drop 0`, `rephase 0`, `tick-late 0 of 119`, `tick-stall 0`, buckets `~1x 119` | **99.9 / 100.0%**, same counters, same buckets |
| 4K H.264 `handler>budget` | **0 of 119** (max 4.3 / 4.4) | **0 of 119** (max 4.5 / 4.4) |
| 4444 cadence x2 | **99.8 / 99.8%**, 261 frames, `drop 0`, `rephase 0`, `tick-late 0 of 260` | **99.8 / 99.8%**, same |
| 4444 `handler>budget` | **0 of 260** (max 33.0 / 33.0) | **0 of 260** (max 33.6 / 33.4) |
| `scrubbar.ps1` full pool | **PASS -- 22 files, 88 legs, `delta 0` throughout**, exit 0, 5 min warm | -- |
| four selftests | green | green, `--window-shape-selftest` identical row for row |

HEAD sits at or below the control's own spread on `handler` max on both files.
The 4444 `<0.9x` bucket reads 2/1 new against 2/2 control, inside that file's
recorded 1-10 span. **Do not chase it.**

**THE SEQUENCE PATH'S BEHAVIOUR IS UNCHANGED, AND THAT HAS ITS OWN CONTROL.**
Both binaries opened the 217-frame PIZ sequence and played for the same window:
both read **`Frame: 124/216 | Seconds: 5.167`**, identical to the frame -- and the
control's HUD carries **no cadence lines at all**, which is the gap this closes,
visible in one capture.

`--window-shape-selftest` reads `1212x682 bound work` for 16:9 rather than the
recorded `1280x720 bound cap`: **identical on the control**, so that is this
display's work area and not a change. Its own PASS line (`OK - 11 shapes x 4
scale factors`) is what the record asserts, and it is green on both.

---

## WHAT THIS MEANS FOR STAGE 3 -- and it changes which file is the test

**The 27-channel DWAA file cannot be used to judge the transform's cost.** Its
handler is already **~190ms against a 41.67ms budget -- 4.6x over** -- with
`handler>budget` at 28 of 28. Adding anything there moves a number that is
already saturated, and a percentage-of-real-time comparison on it would be
measuring the skip mechanism rather than the transform.

**The 217-frame PIZ file is the sensitive test, and the margin is thin.** Its
handler max is **37.4ms of a 41.67ms budget -- about 4.3ms of headroom.** Stage 1
measured the OCIO CPU stage at **9.3 ns/pixel and linear in pixel count**, which
at 1920x1080 is ~19ms single-threaded and roughly 3-4ms in parallel row bands
(the 4K figure was 13.27-14.13ms). **That is the same order as the headroom.**

Two things push the other way, and they are why this must be measured rather than
predicted. With a transform active `ViewerWidget` takes the OCIO branch, which
**skips `measureFloatRange()` entirely** (its own comment says so -- a second full
pass over the frame purely to fill in a report), and it replaces the `Gamma22`
display mapping, itself a full-frame pass with a 255-threshold binary search per
sample. **The net could be near zero or even negative. Nobody knows, and that is
step 3.**

**The ~190ms handler on the DWAA file is an OPEN DISCREPANCY, stated as one.**
`exrprobe --read` measured that file's root RGB pass at **37.98ms standalone**
against the PIZ file's whole handler of 37.4ms -- yet in the app the DWAA file's
handler is ~5x the standalone read. The gap is not explained by anything measured
here. It is not this step's job to explain it, and it must not be guessed at; it
is the first thing an EXR playback pass has to account for.

---

## THE HARNESS

`scripts/measure/seqcadence.ps1`. One or more sequences, N repeats, `TRACE_HUD=1`
and a scratch settings file forced.

**It is NOT `cadence.ps1` and cannot be.** That script crops a **fixed 86px band
330px up from the bottom**, tuned to the video HUD's line count; a sequence HUD
has a different one, so that band lands on the picture. This anchors at the
bottom of the window and takes a generous slice rather than predicting an offset
-- the same lesson `passkeys.ps1` records for its own HUD band, and the same trap
the float-buffer session hit.

The scratch settings file is not optional, for `cadence.ps1`'s recorded reason:
Loop is persisted, and a wrap re-establishes the playback timeline, zeroing
`presented`, `frames`, the histogram and `handler>budget`. A run on a machine
where Loop was left on reports the LAST LAP while every rate figure reads healthy.

**The verdict is the owner's eye on the captures, deliberately.** There is no
recorded baseline to compare against -- that is the whole reason for running it --
so a PASS/FAIL bar would be inventing one.

---

## ONE PRE-EXISTING DEFECT FOUND IN PASSING, RECORDED AND NOT FIXED

**`%%` renders literally on the dev HUD.** `QString::arg` does not collapse `%%`
the way `printf` does, so `(100.0%% real time)`, `media 100.0%%)`, `hit 0.0%%`
and three `io ... seq %%` fields all print a double per-cent sign. **Pre-existing
and widespread** -- confirmed on lines this change never touched, in a capture
taken before it. Cosmetic; fixing it would touch many lines and move every
existing HUD capture, so it is recorded rather than folded into an instrument
commit.

One hardening WAS taken, in the line this change owns: the presented line is
renumbered so `dropField` -- the only argument that is itself a string carrying
per-cent signs -- is substituted LAST. `QString::arg` rescans what an earlier
`arg()` inserted, which is the trap this file already paid for when
`icecream_passes%04d.exr` had its `%04` substituted with a channel count. Nothing
`dropField` can contain is a placeholder today, so it is a guard, not a fix.

---

## Revertability

Three commits, disjoint file sets, no adjacency: the instrument
(`MainWindow.{h,cpp}`), the harness (`scripts/measure/seqcadence.ps1`), this
record. Each reverts cleanly and the reverted tree builds -- checked, not
asserted.
