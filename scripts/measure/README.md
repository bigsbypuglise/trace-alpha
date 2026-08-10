# Scrub / playback measurement harness

Drives a built `Trace.exe` with realistic gestures and captures the HUD, so
"does this feel different" can be answered with the same numbers each time.
Windows only; PowerShell 5.1.

```powershell
$clip = "D:\media\yourclip.mov"
.\scripts\measure\restart.ps1 -Clip $clip          # always start here
.\scripts\measure\play.ps1 -Seconds 9              # playback run
.\scripts\measure\capture.ps1 -Out playback.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Seconds 1.5           # forward sweep
.\scripts\measure\capture.ps1 -Out fwd.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Backward -Seconds 1.5
.\scripts\measure\capture.ps1 -Out back.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Reversals             # the correctness gesture
.\scripts\measure\capture.ps1 -Out rev.png
```

`restart.ps1` finds the exe at `build\app\Release\Trace.exe` relative to the
repo; pass `-Exe` to point elsewhere.

## Run the reversal set, not just a sweep

A single smooth drag scores perfectly on every throughput number and still
misses correctness bugs. `2523d77` — a decode error that put "No decodable frame
at target position" on screen — only appeared under **hard direction reversals
held under one continuous press, running into both ends of the clip**. That is
what `-Reversals` does. It is also the useful stress of buffer recycling: a run
turns over several hundred cache evictions.

## Lifecycle gestures

`lifecycle.ps1` covers the transitions where the decoder changes hands, and
where a bug shows up as a hang, a stale frame or a wrong landing rather than as
a bad number: `-StepCycle`, `-PlayAfter`, `-SwitchMedia`, `-KillMidDrag`.

`-PlayThroughDrag` and `-PausedThroughDrag` cover playback surviving a scrub
(step 5.6). They decide by comparing the picture across a second of wall time,
because "is it still playing" is a question about motion — the frame counter
would need OCR. Modifiers: `-KeyAtRelease` (Space with no settle, so it lands
while the release is still resolving), `-WheelFirst`, `-ToEnd`.

```powershell
.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\lifecycle.ps1 -PlayThroughDrag -Out run.png   # expect moving
.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\lifecycle.ps1 -PausedThroughDrag              # expect still
```

**Restart before EACH through-drag gesture.** They are not composable on one
instance: each either toggles play or does not, so inheriting a playing app
inverts the expected outcome of both, which reads exactly like a product
regression. `lifecycle.ps1` now checks the precondition and fails with
`PRECONDITION FAIL` rather than reporting a confident wrong answer.

**Always run the control.** A check that can only ever report "moving" proves
nothing; the two gestures differ in exactly one thing, whether Play was pressed.
And when a gesture is written for a specific bug, run it against a build that
still has the bug before trusting a pass — `-PlayThroughDrag` reads 0% on
`044b2ea` and 13.3% after the fix, which is what makes the pass mean something.

## Reverse playback

`revplay.ps1` drives continuous reverse (J) and is the only harness that does.
Every reverse figure taken before 2026-08-10 was measuring the J-K-L scheduler
fault instead of reverse playback (plan section 29.2), so treat older reverse
numbers as void rather than as history.

```powershell
.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\revplay.ps1 -Presses 1 -HoldSeconds 9 -Out rev1x.png
```

`-Presses` is how many times J is pressed: 1/2/3 give -1x/-2x/-4x, which is the
whole ladder the engine has today. `-StepCheck` presses Right then Left after
stopping and reports how far the picture moved -- **read both legs**: the `-1`
leg is the landing-exactness result and the `+1` leg is the control that proves
the comparison can see a moved picture at all.

The capture is taken **before** K. The cadence and handler counters survive the
stop but `speed` does not, and a run whose speed cannot be confirmed from its own
capture cannot be quoted.

It clicks to position the playhead rather than dragging: a click is a jump and
lands exactly, while a drag would shuttle every frame on the way and leave the
cache full of the frames the run is about to ask for.

Reverse is silent by design, so unlike the cadence runs this needs no
`TRACE_NO_AUDIO` control -- every file is on the same scheduler already.

`revtransitions.ps1 -All -Clip $clip` covers every way OUT of a reverse run --
K, Space, L, stepping, slider press, media switch, quit -- each on its own app
instance. A reverse run holds the decoder LEASE, and a path that leaves one
without returning it does not misbehave visibly; it strands the decoder on the
worker. That is plan section 29.2's lesson applied in advance: enumerate the
entry points rather than testing the one the harness happens to drive.

Two things it teaches, both learned the hard way:

- **Do not name a PowerShell helper `Diff`.** `diff` is a built-in alias for
  `Compare-Object` and aliases outrank functions, so the helper is never called
  and every verdict in the run is decided by something unrelated to the app.
  `lifecycle.ps1` calls its version `SigDiff` for exactly this reason.
- **The expectation is part of the test.** The first version asserted that
  Space during a reverse run leaves the picture MOVING, and failed the app for
  pausing -- which is what Space has always done to any active playback.

`-Traverse` waits for the picture to stop changing instead of holding a fixed
time. Its sampling resolution is ~200ms, which is too coarse for a fast traverse;
the HUD's own `frames` and `elapsed` are exact and answer the same question.

## Things that will waste an hour if you rediscover them

- **The 1080p validation clip opens on a black frame.** `Universe_rc07_I_9x16_Online.mp4`
  frame 0 is solid black, and it is the default clip for most runs here. "The
  video area is black at frame 0" is therefore **not** evidence of a rendering
  fault — it cost a wrong diagnosis during GATE B (plan §17.2), where a working
  D3D11 path was rebuilt on the theory that Qt was painting over it. Check
  against `4_4K_H264_MP4\Splash_1.mp4`, whose first frame has content, before
  concluding anything from a black picture.
- **To tell "not presenting" from "presenting black", clear to a colour.** The
  red clear is what located the fault in one run: the red appeared with a black
  strip exactly where the video viewport was, which proved present, compositing
  and letterboxing were already correct and moved the search to the pixels.

- **Capture the window at native resolution.** The HUD is unreadable in a normal
  screenshot on a 5120x1440 panel — it downsamples too far. `capture.ps1` uses
  `GetWindowRect` + `CopyFromScreen` for this reason.
- **Restart before locating the groove.** `scrub.ps1` finds the timeline by
  scanning for the track colour RGB(55,55,55), which is the **unfilled** part of
  the track. At the end of a clip the track is fully filled and the scan finds
  nothing.
- **Take the longest run, not the first match.** Window chrome contains runs of
  similar greys; a first-match scan latches onto the title bar and then "drags"
  there. The scan is restricted to the transport band and keeps the longest run.
- **Do not assume a y offset for the transport.** It sits above a HUD whose
  height depends on the media.
- **Spin, don't sleep, between pointer moves.** A synthetic drag that teleports
  the pointer and pauses overstates how well the shuttle keeps up.

## What to read off the HUD

| field | meaning | expected |
|---|---|---|
| `presented X / Y fps (N% real time)` | playback rate | ~99% on the 1080p clip |
| `rep` / `skip` | frames held / dropped | skip **0**; rep a few per 10s |
| `scrub exact target/shown/delta` | frame identity during a drag | `delta 0` |
| `shuttle Nms/f lag Nf` | drag throughput, frames behind pointer | `lag 0f` when settled |
| `smooth gap ... stalls N of M` | paint interval; stalls are the stutter | fewer is better |
| `rev-hit` | frame cache hit rate | 91–94% backward at 1080p |
| `detach` | conversion deep-copies | **0.00**, structurally |
| `stale-blocked` / `recov` | stale-frame and recovery-seek backstops | **0** |
| `renderer` | which backend is presenting | `cpu` today |

`delta`, `detach`, `stale-blocked` and `recov` are correctness figures — if any
of them moves off its expected value, stop and investigate rather than tuning.
Throughput and `lag` say how many frames were produced and how far behind the
pointer the picture is; they say nothing about *when* frames landed, which is
what smoothness is. That is what `smooth`/`stalls` is for.
