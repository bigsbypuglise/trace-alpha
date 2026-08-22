# Audio dropout while the window is dragged — diagnosis (2026-08-21)

Owner report, 2026-08-21: audio drops out while the window is being dragged,
on audio-only files and on video-with-audio; Windows Media Player and another
player do not do it.

**Nothing was built for it, because the named mechanism is refuted and the
symptom did not reproduce in any of the eleven configurations below.** The
harness is `scripts/measure/audiodrag.ps1` and it stays, because the next
report needs it and because a negative result nobody can re-run is not a
result.

Display: **physical panel, 5120x1440 @ 239.999Hz**. Build: local Qt 6.10.2,
the same binary and the same launcher (`Run Trace (mark animation).bat`, which
sets `TRACE_MARK_ANIM=1`) the owner was using.

## The hypothesis, and why it is wrong here

The proposed mechanism was that dragging a window puts Windows into a modal
message loop inside `DefWindowProc` between `WM_ENTERSIZEMOVE` and
`WM_EXITSIZEMOVE`, Qt's event loop does not run for the duration, and
`QAudioSink`'s pull — if serviced from the main thread's event loop — starves.

Both halves fail.

**Qt's event loop DOES run inside the modal move loop.** `WM_TIMER` and posted
messages are dispatched by the loop's own pump, and Qt's Windows event
dispatcher services timers from the window proc. Measured directly: a capture
taken *mid-drag*, with the button still down, reads `wm 0/1/0` — enter fired,
exit had not — and on the same capture the picture is presenting at **23.28 of
24 fps** with `under 0` and `silence 0 B`. The video tick never stopped.

**The sink is not on the main thread anyway.** Qt 6.10's Windows backend is
`QWASAPIAudioSinkStream` (`Qt6Multimedia.dll` exports the name), and that DLL
carries `AvSetMmThreadCharacteristics` and the string `Pro Audio` — MMCSS,
which is only ever applied to a dedicated audio thread. So the pull runs on its
own thread and a blocked UI thread cannot starve it.

## What was measured

Every leg plays for ~3s, captures, performs the gesture, and captures again.
`under` counts pulls the ring could not answer in full; `silence N B` counts
the padding bytes handed to the device, which IS the gap in bytes.

| leg | gesture | result |
|---|---|---|
| drag 4s | caption held, window moving | `under 0`, `silence 0 B` |
| drag 6s | ” | `under 3`, `silence 46872 B` — **end of stream, not the drag** (see below) |
| drag 8s, audio-only | ” | clock tracks wall time |
| drag 10s | ” | `under 0`, `silence 0 B` |
| drag 4s, 8670 pointer moves/s | tight loop, no sleep | `under 0`, `silence 0 B` |
| drag 4s, ±1600px travel | window flown across the whole 5120px desktop | `under 0`, `silence 0 B` |
| drag 4s, `TRACE_RENDERER=cpu` | control on the swapchain | `under 0`, `silence 0 B` |
| drag 8s, audio-only, `TRACE_MARK_ANIM=1` | the owner's own launcher | clock identical to the knob-off run |
| resize 4s | corner held, `wm 125/1/1 size 127` | `under 0`, `silence 0 B` |
| menu 4.4s | File menu held open | `under 0`, `silence 0 B` |
| dialog 4.1s | Go to Frame modal held open | `under 0`, `silence 0 B` |
| idle 4s | the control: play, touch nothing | `under 0`, `silence 0 B` |

**The drag is real and was proved so rather than assumed.** `wm 0/1/1` on the
after-capture is Trace's own `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` counters,
and the harness samples `GetWindowRect` while the button is down: a 4s drag
reads `1925,139 | 1886,121 | 1962,143 | 1891,153 …`, and the ±1600px leg reads
`3516,147 | 364,121 | 3408,144 …`. The window really moved, the modal loop
really ran.

**`under`/`silence` alone would not have been enough, and this is the
methodological point worth keeping.** Both count the RING side: they increment
inside `AudioFeed::readData`, so a device that stops being pulled *at all*
leaves both at 0 while going silent. The corroborating instrument is
`processedUSecs` against wall time — `proc 3546ms → 8846ms` across a 4s drag
plus settle, i.e. **5.30s of audio handed to the device over ~5.3s of wall
time** — and on audio-only media `clk 3.4s → 7.467s (mid-drag) → 12.757s`
across an 8s drag, real time throughout.

**The 6s run's `silence 46872 B` is the one figure that climbed, and it is an
artefact of the clip ending.** `M&M_TopGun_1080.mp4` is 10.05s and 3s of
pre-roll plus a 6s drag plus settle runs past its end; the after-capture reads
`Paused | frame 240` with `state 2`. End-of-stream padding is what
`silenceBytes` is documented to accrue, and re-running the same gesture inside
the clip's length reads `silence 0 B`. **A counter that climbs is not a
confirmation until the run is inside the material.**

## What the drag DOES cost, which is picture and not sound

The move loop is not free, and this is real and repeatable — it is simply not
audio:

| | `drop` | tick jitter max | presented |
|---|---|---|---|
| idle control | `0 (ticks 0)` | 2.22ms | 99.5% |
| resize drag | `0 (ticks 0)` | 13.92ms | 99.5% |
| **move drag** | **66 (ticks 64, max 2, media 129.2%)** | **107.4ms** | **98.1%** |

The same shape appears on `TRACE_RENDERER=cpu` (`rep 2 skip 2`, `sync` max
142.6ms), so it is not the swapchain. A move loop costs the picture a ~110ms
hiccup and a run of single-frame real-time drops; a resize loop — which does
far more UI-thread work, 127 `WM_SIZING` and 181 discarded cache entries — costs
neither. That asymmetry is unattributed and is left that way rather than
guessed at.

## Why the proposed fix was not built

Moving `QAudioSink` to its own `QThread` would not move where WASAPI pulls
from — Qt already pulls on an MMCSS thread — so it buys nothing measurable
here. What it would buy is a cost: `advanceClock()` reads
`sink->processedUSecs()` and `sink->bytesFree()` every video tick, and those
reads would cross a thread boundary. That is the master clock, on the UI
thread, in priority-#1 territory, in exchange for a mechanism that measurement
says does not exist. The device buffer was not touched either; 100ms is a
settled measured value (500ms → 95.3% of real time, 250ms → 97.6%, 100ms →
99.1%) and raising it would trade sync accuracy that was deliberately bought.

## What changed

One line of instrument, and it is the gap this diagnosis kept walking into:
**`under` and `silence` now ride the audio-only HUD line as well as the video
one.** The one media class that is nothing but sound was the class whose HUD
said least about the sound, which is why the audio-only legs above had to be
judged by timing `clk` against wall time instead of read off the screen.

## If it is reported again

Run `audiodrag.ps1` and paste the two captures. The questions in order:

1. Does `silence` climb, **inside the clip's length**? That is the gap, in
   bytes. If it climbs at the end of the clip, that is end of stream.
2. Does `proc` (or `clk` on an audio file) advance by the wall time between the
   two captures? If it stalls, the device stopped being pulled and `under`
   will *not* show it.
3. Does `-Mode idle` at the same duration read the same? Without that control
   a figure is a number, not a comparison.
4. Which Qt is in the build being tested? This diagnosis is Qt 6.10.2. **CI
   pins Qt 6.7.2**, whose Windows audio backend predates
   `QWASAPIAudioSinkStream`, so a release ZIP is not the same experiment as a
   local build and should be tested with `-Exe` pointed at it before anything
   is concluded.
