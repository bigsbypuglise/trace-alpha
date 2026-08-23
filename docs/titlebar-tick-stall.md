# The title-bar hitch: the tick is not delivered for ~500ms at move-loop entry

Owner reproduction, 2026-08-22, screen recording with the HUD up, physical panel.
**Measured, not hypothesised.** This supersedes the framing in
`docs/audio-window-drag.md`, which was correct about what it measured and
looking in the wrong place.

## The symptom, in its corrected form

Press and **HOLD** the real Windows title bar. No movement needed. Picture and
sound hitch together for roughly a quarter of a second, then resume **while the
button is still down**. It is not a drag, it is not sustained for the gesture,
and it is not audio.

## What the recording measures

Frame-differencing the 30fps capture finds three clean freezes during playback:

| freeze | duration | frame-diff on the resuming frame (steady state ~1.0-1.5) |
|---|---|---|
| 3.600s -> 3.900s | **300 ms** | 2.53 |
| 6.200s -> 6.500s | **300 ms** | 2.78 |
| 8.633s -> 8.900s | **267 ms** | 5.87 |

The oversized diff on every resuming frame is the picture **jumping**, not
resuming: the clock kept running through the freeze and the picture catches up
to it. That is the "pauses for a moment then starts playing again" the owner
reported, and it is why the earlier investigation kept finding the clock
healthy either side of the gesture.

## What the HUD proves, and this is the whole finding

Two captures from the same recording, one before the gesture and one **mid-gesture
with the button still down**:

| | before (f105) | mid-gesture (f126) |
|---|---|---|
| `period` last/avg/**max** | 40.70/41.67/**43.84** | 42.66/55.16/**512.36** |
| `handler` avg/max | 0.63/**0.70** | 0.63/**0.77** |
| `jitter` last/avg/max | -0.96/0.76/2.18 | 0.99/14.89/**470.69** |
| `present-late` max | 1.91 | **470.77** |
| `drift` | 0.7ms | **-458.7ms** |
| `presented` | 24.01/24.00 (100.1% real time) | 18.26/24.00 (76.1%) |
| `drop` | 0 | 10 (ticks 4 max 3) |
| `wm` enter/exit | 0/**4**/4 | 0/**5**/4 |

**`period max` is 512ms while `handler max` is 0.77ms.** Trace's per-tick work
did not change by a hair. The tick simply was **not called** for half a second.

`wm 0/5/4` — `WM_ENTERSIZEMOVE` has fired and `WM_EXITSIZEMOVE` has not — proves
the capture is inside the modal move loop, with the button down, and confirms
the gesture is on the **real Windows caption** rather than Trace's own strip.

## Why every previous investigation missed it

- **It is delivery, not work.** Every instrument pointed at Trace's own cost —
  decode, upload, draw, the audio ring — and all of them are innocent. Nothing
  was slow. Something did not run.
- **It is entry-only.** `docs/audio-window-drag.md` measured *mid-drag*, after the
  entry burst, and correctly found the tick presenting at 23.28 of 24fps. Both
  observations are true. The gap is the first ~500ms and nothing sampled it.
- **The smoothness counters are blind to it.** On the same frames that carry
  `period max 539.40`, the HUD reads `stalls 0 of 0 (>33.3ms)` and
  `hitch 0 (>33ms)`. **A 512ms gap did not register as a stall or a hitch.**
  That is a defect in the instrument in its own right and it is why harness runs
  kept coming back clean.
- **Synthetic input never reproduced it.** `audiodrag.ps1` drives the gesture with
  `SetCursorPos` + `mouse_event` and produced a clean negative across eleven
  configurations. A real hand on the caption reproduces it every time.

## What is ruled out, by experiment rather than by argument

| ruled out | how |
|---|---|
| The Qt audio backend / `QWindowsAudioSink` on the old CI Qt | Upgrading to Qt 6.11.2 on both dev and CI (v0.3.0-beta.7) changed nothing |
| Audio, entirely | `TRACE_NO_AUDIO=1` — no sink, no device, no audio clock. **The picture still freezes.** |
| A shallow audio buffer draining | `TRACE_AUDIO_BUFFER_MS=400` changed nothing |
| The top chrome's layered window | `TRACE_TOPCHROME_FADE=0` (never touches the ex-style) changed nothing |
| The per-tick `SetLayeredWindowAttributes` storm | `TRACE_TOPCHROME_ALPHA=255` changed nothing |
| The D3D11 swapchain | `TRACE_RENDERER=cpu` changed nothing |
| Anything system-wide | Holding **Notepad's** title bar while Trace plays does **not** hitch Trace |
| Any mouse press | Press-and-hold in the **centre of the video** does **not** hitch |
| Parsec as a factor | Reproduces identically at the machine and over Parsec |

## Where the mechanism has to live

Between `WM_ENTERSIZEMOVE` and roughly 500ms later, Trace's frame tick is not
serviced. The modal loop inside `DefWindowProc` runs its own message pump, and
`WM_TIMER` is a *synthesised* low-priority message that Windows only generates
when the queue has no input in it. At loop entry the queue is not empty — button
down, `SC_MOVE`, capture change, DWM and Win11 snap-layout traffic — so nothing
low-priority gets a turn until it settles. That is a hypothesis about *why*; the
*where* is measured and is not in doubt.

**The hooks for a fix already exist.** `MainWindow::nativeEvent` already handles
`WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE` and already carries an `inSizeMove_`
flag; today both cases are counters.

**One caution before building.** The obvious answer — start a native `SetTimer`
on entry and drive the tick from `WM_TIMER` in `nativeEvent` — is subject to the
same "only when the queue is empty" rule that is the suspected cause, so it may
buy nothing. **Measure the proposed fix against `period max` before believing
it.** If `WM_TIMER` is genuinely starved at entry, the honest options are to
drive presentation from somewhere that is not the UI thread's message queue, or
to accept the freeze and stop the clock drifting through it so the picture does
not jump on resume.

## Reproduction

Play a video. Press and hold the Windows title bar. Do not move. `H` shows the
HUD. **Read `period max` against `handler max`** — that single comparison is the
whole diagnosis, and any fix has to move `period max` back toward 44ms.

---

# Job 1: the instrument, fixed and proved (2026-08-23)

**The blind spot was not a threshold problem. The counters had no samples.**

`stalls` and `hitch` are fed from exactly two sites, `MainWindow::
paintScrubFrameNow()` and the synchronous scrub walk — **both inside the scrub
drag path**. During ordinary playback there is no drag, so
`scrubPaintGapSamples_` is 0 and every counter derived from it is 0. `stalls 0
of 0` was never a verdict on the gesture; it was an empty measurement, and the
denominator was saying so all along.

That is why a dozen harness runs came back clean. They were not wrong about what
they measured. They were pointed at the drag pipeline while the fault was in the
playback one.

## What changed

- **The line says what it is.** `smooth` is now **`smooth/drag`**, and both the
  member comment and the HUD comment state that the whole line is drag-scoped.
- **Two counters that watch the playback tick**, on the `sched` line, sampled
  from `lastPeriodMs_` — handler entry to handler entry, i.e. **delivery**, not
  work:
  - **`tick-late N of M (>1.5x)`** — the tick arrived so late that at least one
    whole frame opportunity went unused. Relative to the frame budget, so it
    means the same at 24 and 60fps. Same bar as the cadence line's 1.5-2.5x
    bucket.
  - **`tick-stall N (>100ms)`** — absolute, so it is comparable across frame
    rates and sessions the way `hitch` is. 100ms is two to three frames at
    24fps.
  - **`sizemove N max Xms`** — the subset delivered while `inSizeMove_` was
    true, i.e. while `DefWindowProc`'s modal move/size loop owned the message
    pump, with its own max. **This is the attribution.** A count alone cannot
    tell a caption press from an ordinary overrun; this field can.

**33.3ms would have been the wrong threshold and the reason is worth keeping:**
a 33ms *paint gap* during a drag is a stall, while a 33ms *tick period* at 24fps
is early. The two pipelines do not share a bar.

## The proof, A/B, same clip, same build, same duration

`scripts/measure/tickstall.ps1`, `Splash_1.mp4`, `TRACE_NO_AUDIO=1`, 11s.
**Display 1920x1200 @ 59.999Hz — the Parsec-class virtual display, not the
panel.** Legitimate here only because this is the one fault the owner records as
reproducing identically over Parsec; nothing else in this table is a cadence
record.

| | `-Mode idle` (control) | `-Mode caption` |
|---|---|---|
| `tick-late` | **0** of 119 | **1** of 114 |
| `tick-stall` | **0** | **1** |
| `sizemove` | 0, max **0.0ms** | 1, max **298.9ms** |
| `period` last/avg/**max** | 40.31/41.67/**43.28** | 40.86/43.86/**298.94** |
| `handler` avg/max | 1.92/**2.36** | 1.89/**2.48** |
| `wm` enter/exit | 0/**0**/0 | 0/**1**/0 |
| `smooth/drag` `stalls` | 0 of 0 | **0 of 0** |
| `smooth/drag` `hitch` | 0 | **0** |

**The last two rows are the point.** On a capture carrying a measured 298.94ms
gap, the old counters still read a clean sheet — in the same screenshot as the
new ones reading 1. The blind spot is demonstrated rather than argued.

The counters were also shown able to fire on a **work** overrun, so they are not
merely a title-bar detector: the 8K 4444 XQ plate under `TRACE_RT_DROP=0` reads
`tick-late 143 of 143`, `tick-stall 1`, `sizemove 0`, with `period 75.99/81.04/
131.61` against `handler 75.77/78.77`. **That is the opposite shape** — period
tracks handler, `outside 0.22ms` — and it is exactly the discrimination the
instrument exists to make:

- `period max >> handler max` → **delivery**. The tick was not called.
- `period max ~= handler max` → **work**. The tick was called and took that long.

## One correction to the record, and it matters for Job 2

`audiodrag.ps1`'s clean negative across eleven configurations was **not evidence
that synthetic input fails to reproduce this.** `tickstall.ps1 -Mode caption`
reproduces it every run — 302.6ms and 298.9ms on two attempts. The earlier
harness was reading `under` and `silence`, the audio ring, on a fault that is
not audio. **The gesture was almost certainly stalling in those eleven runs and
nothing present was measuring it.**

The synthetic figure is still a **lower bound**, not a substitute: 299-303ms here
against 512ms from a real hand on the physical panel. A clean result from the
script is not evidence the fault is gone. The owner's hand remains the judge.

## What this does NOT do

Nothing about the fault was changed. This commit is the scoreboard only —
`period max` was already telling the truth and now there is a count, a
threshold, and an attribution beside it, so a fix can be judged rather than
believed.

## Addendum: the HUD is not a readable instrument for this (2026-08-23)

Owner, on being asked to read the counters: *"too much text and happens too fast
for me to read the HUD — what I can tell you is the entire HUD pauses when I
click and hold the top chrome, almost as if the entire app pauses."*

**That is itself a measurement and it widens the finding.** It is not the frame
tick alone that stops — the HUD's own refresh stops with it, so the whole UI
thread is unserviced for the duration. Consistent with the delivery diagnosis,
and it constrains Job 2: any fix that lives on the same message queue is
suspect, because the queue is demonstrably not being drained at all.

It also makes the HUD the wrong readout by construction: **the one moment the
number matters is the one moment the display of it is frozen.**

**`TRACE_TICK_LOG=1`** therefore appends one line per late tick to
`%TEMP%\trace_tickstall.txt` — same reasoning as `TRACE_IO_LOG` and
`TRACE_OPEN_LOG`, read afterwards at rest, diffable, no OCR. The header is
written at open, so a run that stalls nothing leaves an empty-but-present file,
which is distinguishable from a run where the knob was never set.

Verified end to end on the synthetic caption hold, and the whole diagnosis fits
on one line:

    t=3.37s period=160.17ms handler=2.04ms budget=41.67ms sizemove=1

160ms of delivery against 2ms of work, inside the modal move loop.

## Owner confirmation of the instrument (2026-08-23) — and a new lead

Twelve holds, physical panel, `TRACE_TICK_LOG=1`, shipping HUD hidden. Raw log
kept at `docs/titlebar-tick-stall-owner-log.txt`. Every line:

    t=4.31s period=558.44ms handler=1.91ms budget=41.67ms sizemove=1
    t=2.50s period=548.59ms handler=2.12ms budget=41.67ms sizemove=1
    t=4.43s period=514.15ms handler=2.16ms budget=41.67ms sizemove=1
    t=6.21s period=508.01ms handler=1.95ms budget=41.67ms sizemove=1
    t=1.70s period=538.77ms handler=2.16ms budget=41.67ms sizemove=1
    t=3.95s period=539.59ms handler=2.20ms budget=41.67ms sizemove=1
    t=6.10s period=521.42ms handler=2.01ms budget=41.67ms sizemove=1
    t=1.79s period=524.03ms handler=2.29ms budget=41.67ms sizemove=1
    t=3.76s period=533.28ms handler=2.15ms budget=41.67ms sizemove=1
    t=5.87s period=520.33ms handler=1.91ms budget=41.67ms sizemove=1
    t=3.40s period=529.28ms handler=2.17ms budget=41.67ms sizemove=1
    t=5.12s period=536.70ms handler=2.11ms budget=41.67ms sizemove=1

**Twelve of twelve, `sizemove=1`, handler 1.91-2.29ms, period 508-558ms.** No
stall in the whole session was anything else — no false positives to filter and
no second cause. The instrument sees the fault, attributes it, and the numbers
land on the doc's own hand-measured 512ms.

(The `t=` values are not monotonic because `sessionClock_` restarts on each
Play; they are position within a run, not a session timeline.)

### THE SPREAD IS TOO TIGHT FOR QUEUE STARVATION, AND 500 IS A REAL NUMBER HERE

`GetDoubleClickTime()` on this box returns **500ms**, and every one of the twelve
stalls is *just over* it: 508, 514, 520, 521, 524, 529, 533, 537, 539, 549, 558.
Range 50ms on a 520ms mean, i.e. **±5%**.

That is the signature of a **timeout**, not of a queue that happens to be busy.
Input traffic varies run to run; a bound does not. It is evidence *against* the
standing hypothesis at the top of this document — "`WM_TIMER` is starved because
the input queue is not empty at loop entry" — because a motionless press
generates almost no input, and because queue congestion would not land twelve
samples inside 50ms of each other.

**It is a correlation, not a mechanism, and it is not yet proven.** Proving it by
changing the system double-click time is a machine-wide settings change and was
not taken. What separates the two families non-invasively is stated in the Job 2
note below.

**Do not build the `SetTimer` + `WM_TIMER` fix off the back of this.** Beyond the
owner's own caution, Qt's Windows event dispatcher already implements `QTimer`
with real Win32 timers, so `playTimer_` is *already* a `WM_TIMER` — a second one
would be the same mechanism that is currently failing, and would buy nothing by
construction. That needs confirming against the Qt 6.11.2 source before it is
stated as fact, but it is the reason the obvious fix is not the first thing to
try.
