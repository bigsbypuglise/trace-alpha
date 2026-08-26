# EXR playback: the pass-cycling leg, and ACES 1.0 against ACES 2.0

Record of what was measured. 2026-08-24, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239999/1000 = 239.999000 Hz**.

**NO PRODUCT CODE CHANGED THIS SESSION.** The only edit is
`scripts/measure/seqcadence.ps1`, which gained a pass-selection leg. There is
therefore no new instrument cost to measure: the instrument built yesterday
(`8271ff2`) was already controlled against `2d09d89` and is recorded there as
free -- both binaries read `Frame: 124/216 | Seconds: 5.167` on the sequence
path, identical to the frame.

---

## THE DISPLAY, CHECKED FIRST

`refresh.ps1` **and** `Get-CimInstance Win32_VideoController`, both, because the
first reports the ACTIVE path and will describe a virtual display without ever
saying the panel changed underneath it.

- Active path **5120x1440 @ 239.999000 Hz** -- the panel, in the mode every
  recorded figure in this project was taken in.
- **`parsecd` IS running** (pids 27024 / 27292) but the Parsec Virtual Display
  Adapter **has no mode set**, and the 4090's own panel is the active path. A
  running daemon is not a session. Said out loud because "Parsec off" and "no
  Parsec virtual display is active" are different claims and only the second was
  verified.

---

## THE PREMISE THAT EXPIRED -- legs 1 and 2 were already measured

The brief for this session was that EXR playback rate "has never been measured
in this project. Not once, across four stages of EXR work." **That was true
until 2026-08-24 and stopped being true the same day**, in two steps recorded in
`docs/exr-stage3-cadence-instrument.md` (the instrument and the baseline) and
`docs/exr-stage3-color-transform-dialog.md` (the transform).

**Sixteenth premise expiry, and the third written by this project's own recent
session.** The pattern is now well enough attested to state plainly: a handoff
paragraph is a claim with a date on it, and the CLAUDE.md carry-forward block
had already struck this one through.

Both legs were **re-run today** rather than quoted, so the whole set below is
same-session and shares one control.

---

## LEG 1 -- BASELINE, NO TRANSFORM (re-run, 3 reps each)

### `R2_OP_Stacks_01` -- 217 frames, 1920x1080, 3ch, PIZ

| | rep 1 (cold) | rep 2 | rep 3 | recorded 2026-08-24 |
|---|---|---|---|---|
| presented | 23.53 (**98.0%**) | 23.97 (**99.9%**) | 23.96 (**99.8%**) | 99.9 / 99.9% |
| frames / elapsed | 213 / 9.05s | 216 / 9.01s | 216 / 9.01s | 216 / 9.01s |
| skip | 4 (media 99.9%) | **0** (99.9%) | **0** (99.8%) | 0 |
| handler>budget | 4 of 212 (max 58.6) | **0 of 215** (max **37.8**) | **0 of 215** (max **37.2**) | 0 of 215 (37.4 / 36.6) |
| cadence p50 / max | 41.7 / 99.4 | 41.6 / 45.6 | 41.8 / 45.9 | 41.6-41.7 / 45.8 |

**Reproduces the record to the digit on the warm reps.** The cold first rep
reads 98.0% with `skip 4` -- the recorded cold-sweep lesson, arriving again.

### `MultlayerAces` -- 97 frames, 1920x1080, 27ch, DWAA (lossy)

| | rep 1 | rep 2 | rep 3 | recorded |
|---|---|---|---|---|
| presented | 6.87 (**28.6%**) | 6.99 (**29.1%**) | 7.03 (**29.3%**) | 28.8 / 29.0 / 29.2% |
| frames / elapsed | 28 / 4.07s | 29 / 4.15s | 29 / 4.13s | 28-29 / ~4.1s |
| **skip** | **68** (media 98.2%) | **70** (99.4%) | **68** (97.9%) | 68-70 |
| handler>budget | 27 of 27 (max 190.2) | 28 of 28 (max 189.7) | 28 of 28 (max 186.0) | 27-28 of 27-28 |

Same sentence as the record: this file plays 97 frames of media in ~4.1s **by
showing 29 of them and skipping 68.** `media ~98%` and `real time ~29%` are both
true and are different questions.

---

## LEG 2 -- THE VIEW TRANSFORM, AND THE ACES VERSION IS THE VARIABLE

**THIS IS THE SESSION'S LARGEST RESULT AND IT IS NEW.** The record measured
**ACES 2.0** from the built-in `ocio://default`. The brief asked for
**`config.ocio`, display `sRGB`, view `ACES 1.0 SDR-video`** -- the asset set's
own Redshift config, and a *different transform*. It costs about half as much.

`R2_OP_Stacks_01`, the sensitive file, 3 reps:

| transform | presented | skip | handler>budget | handler max |
|---|---|---|---|---|
| **none** (control, same session) | **99.9 / 99.8%** | 0 | **0 of 215** | **37.2-37.8 ms** |
| **ACES 1.0 SDR-video** (`config.ocio`) | **71.0 / 68.7 / 70.3%** | 62 / 67 / 64 | 74 of 153 - 80 of 148 - 67 of 152 | **63.9-73.7 ms** |
| ACES 2.0 SDR 100 nits (`ocio://default`) | **36.4%** | 138 | 79 of 79 | **129.5 ms** |
| a `.cube` LUT | **100.0%** | 0 | 0 of 215 | 28.4 ms |

**Per-frame added cost, read off handler max against the same-session control:**
LUT **cheaper than none** (it replaces `measureFloatRange()` and the `Gamma22`
threshold search) - ACES 1.0 **~ +32 ms** - ACES 2.0 **~ +92 ms**.

**So "an ACES view transform costs ~92 ms" is an ACES 2.0 statement, not an ACES
statement.** ACES 1.0 through the identical plumbing -- same float path, same
parallel bands, same display stage, same config file the assets ship with --
lands at 68.7-71.0% of real time rather than 36.4%. Neither holds 24 fps, and
the gap between them is larger than the gap between ACES 1.0 and holding rate.

`MultlayerAces` for completeness, and it **decides nothing** -- it is already
4.6x over budget with `handler>budget` saturated, so anything added there moves
a number that cannot move:

| transform | presented | skip | handler max |
|---|---|---|---|
| none | 28.6 / 29.1 / 29.3% | 68-70 | 186.0-190.2 ms |
| ACES 1.0 | 25.4 / 25.7 / 25.9% | 71-73 | 205.1-212.0 ms |
| ACES 2.0 (recorded) | 20.6% | 78 | 231.6 ms |

**THE KNOB WAS READ BACK, NOT TRUSTED.** Every ACES capture carries
`map OCIO config.ocio / sRGB / ACES 1.0 SDR-video` on its own media line. A null
result from a knob whose state cannot be read off the running build is not a
result -- the recorded `fastmove off` lesson.

---

## LEG 3 -- PASS CYCLING, AND IT IS FLAT

Genuinely new; no pass-cadence figure existed. `MultlayerAces`, no transform,
3 reps each, the pass selected **before playback starts** (`applyExrPass()`
clears the frame cache, so cycling mid-run would fold a cache refill into the
figure).

| pass | standalone read (`exrprobe`) | presented, 3 reps | handler max |
|---|---|---|---|
| **1 (root)** colour, Gamma 2.2 | 35.35 ms | **28.6 / 29.1 / 29.3%** | 186.0-190.2 |
| **6 (P)** position, Normalise | 34.96 ms | **29.4 / 29.4 / 29.2%** | 185.5-187.5 |
| **9 (SpecularLighting)** colour, Gamma 2.2 | **45.41 ms** | **29.1 / 28.1 / 28.5%** | 182.4-194.4 |

**The pass makes no measurable difference.** The whole population spans
28.1-29.4%, and pass 9's own three reps span 28.1-29.1% -- so the between-pass
difference is smaller than the within-pass variance.

**AND THAT NARROWS THE OPEN DISCREPANCY, which is why the leg was worth running.**
Step 1 recorded that this file's ~190 ms handler is **~5x its own standalone
read** and left it unexplained. Passes 1 and 9 are both colour passes through the
identical display mapping and differ by **~10 ms of read, a 29% difference** --
and **none of it reaches the handler.** So the ~150 ms that is not the read is
**invariant to which channels are read**: it is a fixed per-frame cost, not a
function of the channel span, and any future EXR playback pass should look for it
somewhere other than the channel-reading path.

Pass 6 also confirms the pinned `Normalise` range on screen at every rep --
`map Normalise [-44.2500..44.2500, 66.3% >1]`, identical across reps, which is
the stage-2 per-frame auto-range defect staying fixed.

---

## THE HARNESS, AND IT WAS PROVEN ABLE TO FAIL BEFORE IT WAS BELIEVED

`seqcadence.ps1 -PassAdvance N` presses `]` N times between the open and
playback, and **asserts the press landed** by capturing the HUD band either side
and comparing. Taken while PAUSED, so nothing ticks, the three cadence lines are
frozen, and a zero reading can only mean the key did not act -- never that the
app was merely idle.

**Two thresholds with a gap** (0.50 / 2.00), never one cutoff, and a reading
between them fails as *inconclusive* rather than being rounded toward the
expected answer -- `passkeys.ps1`'s recorded lesson.

**Three controls, run before any positive result was trusted:**

| control | band moved | verdict |
|---|---|---|
| PIZ file (one pass; the action is disabled) | **0%** | NO-CHANGE, correctly |
| DWAA + unbound key `z` (key lands, nothing bound) | **0%** | NO-CHANGE, correctly |
| DWAA + `]` (positive) | **2.157%** | pass 2/9 Beauty, confirmed on the capture |

The first two each printed `picture 12-73% changed -> advancing` in the same
run, so the 0% is specifically the pass not changing and not a dead app. **A run
whose keypress went nowhere would report every pass with identical figures --
which reads as "the pass makes no difference", i.e. exactly this session's
positive conclusion.** That is why the controls came first.

### Two harness facts worth carrying

**`passkeys.ps1`'s band is now STALE and will misread.** Its `Grab-Hud` takes the
**last** HUD line on the stated grounds that the last line is the media line
carrying `pass N/M`. Since `8271ff2` the sequence HUD appends **three cadence
lines after the media line** (deliberately -- "so the media line stays first and
every existing sequence-line capture keeps its geometry"). `passkeys.ps1` was
validated before that commit. **Reported, not fixed**, because this session was
scoped to measurement; the fix is its `rows` argument, and it should be re-run
and re-validated before its verdicts are quoted again.

**A single unescaped `]` per `SendWait` IS delivered.** The recorded trap says
brackets are "silently swallowed"; measured here, one lone `]` reached Trace and
advanced the pass. The recorded observation came from a **ten-press leg**, i.e.
repeated brackets inside one `SendWait` string. The rule that survives is the
narrow one: **escape them anyway** (this harness sends `{]}`, one per call), but
an unescaped bracket is not a reliable negative control -- which is why the
unbound-key control above replaced it.

---

## WHAT THIS DOES AND DOES NOT DECIDE

**It does not decide anything on its own. Owner call, deliberately not taken here.**

- **PIZ, no transform, holds rate** -- 99.8-99.9%, `skip 0`, `0 of 215`,
  ~4 ms of headroom. Nothing to optimise for the plain EXR review case.
- **The 27-channel DWAA file does not, and never did** -- 29% of real time,
  saturated, and **not fixable by pass selection**.
- **The transform is where the cliff is, and its steepness is the ACES version**:
  a LUT is free, ACES 1.0 loses ~30% of real time, ACES 2.0 loses ~64%.
- **The window cache is untouched by any of this** and its byte-bounding question
  (8K would want ~1.6 GB) is unaffected -- no 4K or 8K EXR exists in the pool, so
  that remains arithmetic rather than measurement.
