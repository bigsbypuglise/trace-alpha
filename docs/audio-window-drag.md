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

---

# Re-tested on the SHIPPED beta.6 ZIP (2026-08-22)

Owner re-reported the dropout on 2026-08-22, against the **release ZIP**, and
raised the live lead this document's own closing section asks for: the
2026-08-21 diagnosis ran on a **local Qt 6.10.2** build, and **CI pins Qt
6.7.2**, whose Windows audio backend predates `QWASAPIAudioSinkStream`.

**The lead's premise is CONFIRMED. Its conclusion is REFUTED — on new evidence,
not on the old reasoning. Nothing was built, and neither proposed fix is funded
by measurement.**

## The premise was right, and the 2026-08-21 refutation does not transfer

Byte-searched in the two binaries themselves rather than inferred from a version
number:

| | `Qt6Multimedia.dll` | `QWASAPIAudioSinkStream` | `AvSetMmThreadCharacteristics` | `Pro Audio` | `QWindowsAudioSink` |
|---|---|---|---|---|---|
| **beta.6 ZIP** | **6.7.2.0**, 864,400 B | **0** | **0** | **0** | 3 |
| local build | 6.10.2.0, 1,241,400 B | 8 | 2 | 1 | 2 |

So the shipped binary has **no MMCSS audio thread at all**. The sentence
"the sink is not on the main thread anyway" is a fact about 6.10 and is **false
for the build the owner runs**. That half of the earlier diagnosis is withdrawn.

## The conclusion survives, measured on the 6.7.2 binary itself

`audiodrag.ps1 -Exe <the ZIP's Trace.exe>`, `-Tight -Amplitude 1600` — the
window flown across the whole 5120px desktop at ~7,700–8,500 pointer moves/s,
far harsher than a 1000Hz hand. Physical panel, 5120x1440 @ 239.999Hz.

| binary | Qt | gesture | Δ`proc` | Δ`clk` | `under` | `silence` |
|---|---|---|---|---|---|---|
| **ZIP** | 6.7.2 | drag 4s | 5250ms | 5.290s | 0 | **0 B** |
| **ZIP** | 6.7.2 | idle 4s (control) | 5310ms | 5.342s | 0 | **0 B** |
| local | 6.10.2 | drag 4s | 5330ms | 5.337s | 0 | **0 B** |
| local | 6.10.2 | idle 4s (control) | 5250ms | 5.251s | 0 | **0 B** |
| **ZIP** | 6.7.2 | audio-only drag 8s | — | 9.281s | 0 | **0 B** |
| **ZIP** | 6.7.2 | audio-only idle 8s | — | 9.258s | 0 | **0 B** |
| **ZIP** | 6.7.2 | **audio-only drag 25s** | — | **26.304s** | 0 | **0 B** |

**Drag and idle are indistinguishable on both Qt versions**, and every run was
taken inside the clip's length, per this document's own trap.

**The decisive reading is the mid-drag capture, taken with the button still
down and `wm 0/1/0` proving the modal move loop was running:** on the 6.7.2 ZIP
the audio clock advanced **`clk 3.501s → 5.501s`, exactly 2.000s over the first
2.0s of the drag**. On the 25s audio-only leg, `clk 3.500 → 15.970 → 29.804` —
real time at the midpoint and at the end. **The feed never stalls, on either Qt.**

**Why**, and this is the mechanism that makes the earlier conclusion right for
the wrong reason: **Qt's event loop runs inside the modal move loop** —
`DefWindowProc`'s own pump dispatches `WM_TIMER` and posted messages — which the
2026-08-21 session already measured for the *video* tick. A main-thread,
timer-driven sink pull is therefore serviced too. **MMCSS was never what was
saving it.** Corroborating: on the ZIP the sink's `free` *rose* 3840 → 15360 of
38400 across the drag, i.e. the device buffer was **draining** — something
downstream kept consuming.

## Therefore neither proposed fix is funded

- **Bumping the CI Qt pin to 6.10.2** would not fix this. 6.10 is measured here
  and behaves identically; the bug is not a 6.7-vs-6.10 difference in the feed.
  It also reopens window shaping, DPI and imageformats across a Qt minor.
- **Driving the sink from its own thread** addresses a starvation that
  measurement says does not occur on either backend, and would put
  `advanceClock()`'s per-tick `processedUSecs()`/`bytesFree()` reads across a
  thread boundary — the master clock, in priority-#1 territory. The 2026-08-21
  rejection stands, on **new** grounds: not "6.10 already has an MMCSS thread",
  but "the feed is provably continuous through the drag on the build that fails".

The 100ms device buffer was not touched.

## The gap that is left, and it is an instrument gap

**Every counter Trace has stops at `QAudioSink`.** `proc` counts bytes Trace
handed to Qt; `under`/`silence` count the ring side. **None of them can see
whether WASAPI actually emitted the sound.** If the dropout happens below that
boundary, every figure in the table above reads clean and the owner still hears
silence — which is exactly the situation.

Two things point that way. **The owner's own phrasing: "resumes in sync."**
Trace's ring is a FIFO. If Trace had stopped feeding, resuming would put audio
*behind* by the length of the drag and it would return **out** of sync. Audio
that goes away and comes back in sync is audio that **kept being consumed while
it was not audible**. And the draining `free` above says the same thing from the
other side.

**The output could not be recorded from here.** There is no loopback capture
device on this box — `ffmpeg -f dshow -list_devices` finds only
`Microphone (Steam Streaming Microphone)`, and making a loopback would mean
changing the default render endpoint, which is a system settings change *and*
would change the experiment being measured.

## What the next session needs, and it needs ears

`18_Audio/tone_440_click_90s.wav` was generated for this: 90s of a steady 440Hz
tone with a 1600Hz click every second. A dropout in a tone is unmistakable where
one in music is easy to mislocate, and the clicks make the gap **countable by
ear** — "I lost four clicks" turns a report into a duration.

Hand test, HUD shown (`H`), read the audio line before and after:

1. **Hears silence, and `clk`/`proc` advanced by the wall time, `silence 0 B`**
   → the gap is **below `QAudioSink`**. Neither fork applies. The investigation
   moves to the endpoint and to Qt's sink internals.
2. **Hears silence, and `clk`/`proc` stalled** → the feed did stop and the
   synthetic gesture is simply not the owner's hand. Fork (b) becomes live and
   this table needs re-reading.
3. **Hears no silence on the tone** → the fault is media- or state-specific.

**Also needed and not obtainable from here: which output endpoint is default.**
This box has `NVIDIA High Definition Audio` (DisplayPort to the Odyssey G95SC),
`NVIDIA Virtual Audio Device`, `Steam Streaming Speakers` and
`High Definition Audio Device` all active. A virtual or GPU-attached endpoint is
a materially different animal from a motherboard codec, and "other players do
not do it" can be true of a Trace-specific WASAPI usage on such an endpoint.
Enumerating the default from PowerShell 5.1 failed — a `__ComObject` will not
cast to an `Add-Type` `[ComImport]` interface; do it in an all-C# static method
or read it off Windows sound settings.

## Two harness lessons

- **`Start-Process`-launched Trace occasionally reports "no window"** on the
  first try — the recorded flake class; re-run before believing it.
- **`… | Select-Object -First N` on a harness pipeline TERMINATES THE HARNESS.**
  PowerShell throws `StopUpstreamCommandsException` upstream, so a run filtered
  that way never reached its after-capture and silently produced a partial
  result set. Redirect to a file and filter afterwards.

## CORRECTION, same day: there IS a measurable fault, and two instruments were lying

The owner then reported that **Windows Media Player, on the same monitor speakers
and the same file, does not blip while its window is dragged, and Trace does.**
That removes the endpoint as the explanation and forced a re-reading of the
instruments above. Two of them do not say what this session first claimed.

**`clk` cannot see an audio stall, and reading it as if it could is a stale
instrument.** `AudioOutput::advanceClock()` is a **wall-clock projection**
(`clockBase + wall`), slewed 5% per update toward the raw audio position and then
monotonically clamped. It advances at real time whether or not the device is
being fed — that is what it was built for. **Every audio-only leg in the table
above rests on `clk` alone, because `proc` and `snap` were not on the audio-only
HUD line.** Those legs are withdrawn as evidence.

**A before/after `proc` delta cannot see a sub-second gap.** It is sound for a
drag-length stall — a 4s silence inside a 5.3s window cannot hide in a 1%
agreement, so the video legs above do still exclude *that* — but a 150ms gap
moves a 5.3s total by 3%, inside the run-to-run noise.

### The instrument that was missing, and what it found

`AudioFeed::readData` now records the interval between consecutive device pulls:
**`pull max Xms over50 N dry M`** on both the video and the audio-only HUD lines,
plus `proc`, `snap` and `state` on the audio-only line, which had none of them.
`over50` is the normal cadence (the sink pulls about every 50ms — half the 100ms
device buffer) and is context; **`dry` counts gaps wider than the whole device
buffer, i.e. the endpoint had nothing to play.**

Local build (Qt **6.10.2**), `tone_440_click_90s.wav`, physical panel:

| leg | `proc` | `snap` | **`pull max`** | `over50` | **`dry`** |
|---|---|---|---|---|---|
| **idle 10s (control)** | 14796ms | x0 | **50.8ms** | 55 | **0** |
| drag 10s, 34 moves/s | 14726ms | x0 | **153.4ms** | 56 | **1** |
| drag 10s, 8488 moves/s | 14656ms | x0 | **131.3ms** | 43 | **1** |
| **drag 30s**, 35 moves/s | 34856ms | x0 | **144.3ms** | 107 | **1** |

**Idle never exceeds 50.8ms and never runs dry. Any drag — even a gentle one —
produces one ~150ms pull gap and one dry device.** `under 0`, `silence 0 B` and
`snap x0` throughout, which is exactly why every previous pass missed it: the
ring always had data whenever it was finally asked, so the ring-side counters
stay clean while the *endpoint* starves.

**`dry` is 1 at 10s and 1 at 30s.** The cost is a one-off at the transition into
the modal move loop, not a continuous starvation — **on Qt 6.10**. That is one
audible blip per drag, which matches "blip/cut" and does **not** match the
original "silent for the length of the drag".

### What this does to the fork

The 6.10-vs-6.7 question is **re-opened, not settled**. The shape of the fault on
6.10 — one gap at the mode change, then immediate recovery — is what a pull on a
dedicated MMCSS thread would look like. A pull driven by a `QTimer` on the main
thread, which is what 6.7's `QWindowsAudioSink` is, has no reason to recover
until the modal loop ends, and *that* would be a drag-length silence.

**This cannot be measured here: only Qt 6.10.2 is installed on this box, so the
instrumented binary is 6.10 and the shipped one is 6.7 and uninstrumented.**
The next step is an instrumented **6.7.2** build — either from CI (which pins
6.7.2) or from a local `aqtinstall` of 6.7.2 — run through the same three legs.
If 6.7 reads `dry 1` like 6.10, the Qt version is not the variable and the
one-blip transition cost is the whole bug. If it reads `dry` climbing with drag
length, fork (a) is the answer and this is why.

**Do not bump the Qt pin before that measurement exists.** The one-blip
transition cost is present on 6.10 and would survive the bump.

### The move loop is the only gesture that starves the device

Same build, same clip, same 10s hold, one leg per modal gesture:

| gesture | `pull max` | **`dry`** |
|---|---|---|
| **idle (control)** | 50.8ms | **0** |
| **move drag** | **131.3 – 153.4ms** | **1** |
| resize drag | 79.7ms | **0** |
| File menu held open | 51.2ms | **0** |
| Go to Frame modal held open | 50.6ms | **0** |

**Only the move drag runs the device dry.** The resize drag is the same class of
Win32 modal size/move loop and does *far* more UI-thread work inside it — ~127
`WM_SIZING`, the §4 aspect lock on every one, a scrub-preview resync, ~181
discarded cache entries — and it never exceeds 79.7ms. The two Qt nested event
loops sit on the idle floor.

**So "the modal loop blocks the thread" is not the mechanism**: the gesture that
blocks it hardest is clean. This is the *same asymmetry the 2026-08-21 pass
measured on the picture side* and left unattributed — a move drag costs
`drop 66` and 107ms of jitter where a resize drag costs neither. One cause is
now known to hit both sound and picture, which is more than was known before.

**The gap is at the START of the drag and never recurs.** The mid-drag capture of
the 30s leg, taken at 15s with the button still down, already reads
`pull max 144.3ms` and `dry 1` — the final values. A 10s drag and a 30s drag both
end at `dry 1`. On Qt 6.10 this is a one-off cost of *entering* the move loop,
not a starvation sustained through it.

**Pushed as `diag/audio-pull-gap-instrument` (`6f4bed3`) so CI builds it against
the Qt 6.7.2 it pins.** That artifact is both the 6.7 measurement and the owner's
hand test, on the exact version that ships and that the report is against.

## THE 6.7.2 ANSWER: the Qt version is NOT the variable, and BOTH forks are dead

CI run **32599463708** built `b15aeeb` against the Qt it pins. Verified in the
artifact before measuring anything: `Qt6Core`/`Qt6Multimedia` **6.7.2.0**,
`AvSetMmThreadCharacteristics` **0**, `QWASAPIAudioSinkStream` **0**,
`QWindowsAudioSink` 3 — the shipped backend, no MMCSS thread — and the
instrument present (`pull max` ×2, ASCII; a UTF-16 search finds nothing, these
are plain `QString` literals in `.rdata`).

Same clip, same panel, same harness, one leg per row:

| leg | **Qt 6.7.2 (ships)** | **Qt 6.10.2 (local)** |
|---|---|---|
| idle 10s (control) | 52.2ms · **dry 0** | 50.8ms · **dry 0** |
| **move drag 10s** | 142.5ms · **dry 1** | 153.4ms · **dry 1** |
| **move drag 30s** | 155.4ms · **dry 1** | 144.3ms · **dry 1** |
| resize drag 10s | 75.6ms · **dry 0** | 79.7ms · **dry 0** |
| menu held 10s | — | 51.2ms · **dry 0** |
| modal held 10s | — | 50.6ms · **dry 0** |
| move drag, `TRACE_RENDERER=cpu` | 150.2ms · **dry 1** | — |
| move drag, `TRACE_MARK_ANIM=0` | 169.6ms · **dry 1** | — |

**6.7.2 reads `dry 1` at 10s and `dry 1` at 30s — identical to 6.10.2. It does
not stall for the length of the drag.** Both Qt versions cost exactly one
~150ms pull gap on entering the move loop and then recover for as long as the
drag lasts.

**Fork (a), bumping the Qt pin, is refuted.** The version is not the variable and
the bump would carry the fault across unchanged, while reopening window shaping,
DPI and imageformats for nothing.

**Fork (b), driving the sink from its own thread, is refuted by the same table
and this is the stronger result.** Qt **6.10 already does exactly that** — a
dedicated MMCSS audio thread, confirmed in the DLL — **and gaps identically.**
So the UI thread being blocked is not the mechanism, and moving the sink off it
buys a thread boundary on the master clock in exchange for a fault that survives
having one.

**Neither fix is funded. The Qt pin and the 100ms device buffer stay as they are.**

### What the fault actually is, and what is still unattributed

One ~150ms gap in the device pull, at the moment the modal **move** loop is
entered; never repeated however long the drag runs; `under 0`, `silence 0 B`,
`snap x0` throughout, so every pre-existing counter reads clean. Present on both
Qt versions, on **both renderers**, and with the mark animation **off** — so it
is not the flip-model swapchain, not the compositor path and not the animation
timer. Absent from the resize loop, which is the same class of Win32 modal
size/move loop doing far more UI-thread work inside it, and absent from both Qt
nested event loops.

**What costs the 150ms is not yet attributed, and is deliberately left that way**
rather than guessed at. The one thing now excluded that was not before is the
whole "blocked UI thread" family, on the 6.10 MMCSS evidence.

### The gap between this and the report

One dry event is **one blip**, not "silent for the length of the drag". It
matches the owner's later "blip/cut" and not the original wording. **Whether a
real hand drag produces more than the synthetic one does is still open, and is
now readable rather than a judgement call**: the CI artifact prints `pull max`
and `dry` on the HUD for video and audio-only alike, so a hand drag on the work
machine answers it directly. `dry` climbing past 1 during a hand drag would mean
the synthetic gesture is milder than the real one and this table needs re-taking.
