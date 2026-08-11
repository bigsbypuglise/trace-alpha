# Interface pass 1 — phase record

One section per shipped phase. The phase 1 audit is its own document
(`docs/interface-pass-1-audit.md`) and is read-only; this is where what actually
shipped, and what the shipping changed about the plan, gets written down.

Every phase carries its own playback and scrub regression, because priority 1
outranks the pass and phase 14 is too late to find out.

---

## Phase 2 — shared actions and transport artwork integration (`58bfca6`, 2026-08-10)

### What shipped

**Fullscreen is a shared QAction.** `fullscreenAction_`, checkable, created in a
new `setupSharedActions()` that runs *before* `setupMenus()` — a menu adds an
action, it never defines one. The File menu, the transport button and three
shortcuts all reach the same object, and `toggleFullscreen()` is the single place
the window state changes. It re-reads `isFullScreen()` afterwards rather than
trusting the toggle, so the action's tick, `viewState_.fullscreen` and the button
icon all come from what the window manager actually did.

**F11 is listed first among its shortcuts, and that is load-bearing rather than
cosmetic.** Qt advertises only the first sequence — in the menu, and in whatever
a tooltip is written to say. Ctrl+Return is kept because the spec's own rule is
to preserve an existing binding on conflict, and Alt+Enter is the spec's second.
Escape-exits, geometry save/restore and the monitor rule are **phase 6**: they
change what the toggle *does*, and this phase only changed how many places
define it.

**The dev HUD toggle is a shared QAction on `H`** (owner request, 2026-08-10),
with Return/Enter kept as the binding it has always had.

**`showInfo` is deleted, not wired up.** `Qt::Key_I` toggled it and *nothing read
it*, so pressing `I` repainted and changed nothing — a flag with a shortcut and
no reader reads as a broken feature rather than as an absent one. There is one
HUD and it is `showHud`; the spec claims `Ctrl+I` for the Movie Inspector, so
leaving plain `I` as a second, differently-scoped info toggle would have created
an ambiguity for nothing. `showTimecode` and `showSeconds` were dead in exactly
the same way and went with it.

**Hidden now means NOT BUILT.** `refreshHud` returns before formatting several
hundred bytes of `QString` for a widget nobody can see. The pre-existing Return
binding hid the widget and built the line anyway, so "hide the HUD" cost exactly
nothing before this — and the owner's stated reason for wanting the key is to
judge feel *without the instrument*, which means without its cost.

The telemetry capture still runs, and that is deliberate: it is a struct copy,
and it feeds `ra-walk` and the seek counters. **A counter that measures something
different depending on whether the HUD happened to be visible** is the failure
this project keeps re-learning, and it would have been introduced here for a
saving of nothing.

**One icon source.** The `260807` package supersedes both older ones and says so
in its own `CONTENTS.txt`. `assets/icons/` is deleted (app icon; the new package
ships identical filenames, so it is a path change). App icon, play, pause and
both fullscreen glyphs now come from the approved set, along with its state
opacities — rest 0.82, hover fill 0.09, pressed 0.17, disabled 0.28.

**Its control geometry is deliberately NOT adopted.** The approved package
specifies 34×34 utility targets and a 44×44 play/pause inside a rounded panel,
but that is the geometry of the **floating transport**, which is what phase 6
puts on screen when it removes `transportBar_` from the layout entirely.
Re-laying-out a widget that is about to be deleted is churn the spec does not ask
for, and it would move the video rect — and therefore invalidate every scrub
baseline — for a component with no future.

### The rule the artwork follows, and why one directory survives

**Artwork follows behaviour.** The approved package contains no frame-step icon
*by design* ("Left/Right Arrow handle single frames"), and its
`transport_scan_*` pair is the artwork for the **redesigned** Rewind and
Fast-forward — controls that still perform single-frame stepping until phases 4
and 5. Putting scan artwork on a stepping button would put a lie on a visible
control in a player whose third pillar is that it can be trusted.

So `assets/Interface/` survives for **exactly two glyphs** and leaves with them
at phases 4–5. `transport_scan_*` is embedded in the `.qrc` now, unused, so those
phases are a code change only.

The same rule fixed something in the composited overlay rather than preserving
it. Play/pause took the approved glyphs; the two side regions took the
**frame-step** glyphs, matching the actions they actually call. Before this the
overlay drew continuous-scan chevrons over stepping behaviour and the mismatch
was *recorded* rather than fixed — the right call while the art was placeholder,
and the wrong state to keep once it is real. Both halves move together at phases
4 and 5.

The atlas is still rasterised once per size change, into a cell exactly the size
of the snapped destination rect, so the 1:1 blit and the measured cross-backend
agreement are untouched. Confirmed below rather than argued.

### Regression

Control binary built from `87a39a6`; **both binaries measured on the physical
panel** (5120×1440 @ 239.999Hz), `d3d11` default, `win 1280x843` / `1280x829`.

The display changed from Parsec's 1920×1200 @ 60Hz to the panel *part-way
through this session*, which invalidated a baseline already taken — so the
control was rebuilt and re-measured rather than compared across displays.

| run | control | phase 2 |
|---|---|---|
| 4K H.264 playback | 99.1%, 120/120, `handler>budget 0 of 120` (max 3.8), `hitch 0` | 99.1%, 120/120, `0 of 120` (max 3.9), `hitch 0` |
| 4444 playback | 99.7%, 223 frames, `0 of 222` (max 38.1), `hitch 0` | 99.7%, 223 frames, `0 of 222` (max 35.9), `hitch 0` |
| 4K H.264 reversals | `hitch 1`, `delta 0`, `rev-hit 98.6%`, `ui gap max 85.4ms` | `hitch 1`, `delta 0`, `rev-hit 98.7%`, `ui gap max 84.7ms` |
| 4444 reversals ×3 | `hitch 12, 12, 8` | `hitch 7, 10, 10` |
| `-SnapRelease` | — | `target 120 shown 120 delta 0`, full-res, `hitch 0` |
| lifecycle | — | `-PlayThroughDrag` PASS 40.1% moved, `-PausedThroughDrag` PASS 0% |
| revtransitions | — | all six shuttle exits PASS |
| overlay, 12 states | cpu-vs-d3d11 **312 px (0.619%), max delta 24** | **312 px (0.619%), max delta 24** |

Cadence buckets are identical on both files. Landing is exact on every run.

**The overlay diff is the same number to the pixel on both binaries**, which is
what says the artwork swap changed the cross-backend agreement by nothing. The
0.619% is pre-existing and is the video picture's own backend difference inside
the sampled band, not the panel.

**One number moved and it was noise.** 4444 drag `p2p max` read 151ms on one
control run and 1823ms on one phase-2 run — a 12× difference on the noisiest
statistic available (single worst sample, on the file with 13% supply). Three
runs each: control **151 / 1953 / 1148**, phase 2 **1823 / 883 / 733**.
Overlapping ranges, and a 13× spread *inside the control alone*. There is also no
mechanism: the overlay is off by default so `OverlayModel::buildFrame` returns
before doing anything, and `QAction::setChecked` early-outs on an unchanged
value.

### What phase 2 changes about the plan

**The handoff note's safeguard for the `H` toggle does not work, and the note
said it did.** It predicted — correctly — that hiding the HUD would move stall
counts, and concluded that the existing discipline covers it "because `win WxH`
changes with the toggle".

**It does not change.** Measured on the same 4K H.264 reversal drag, HUD shown
against HUD hidden: `win 1280x843` **both times**. The window does not resize.
The *viewer* takes the HUD's height, so what moves is the **video rect**, which
the HUD reports as `display`:

| | `win` | `display` | `paints` | `stalls` | `hitch` | cache |
|---|---|---|---|---|---|---|
| HUD shown | 1280x843 | 640x360 | 374/375 | 70 of 370 | 1 | 197/278 (272.0MB) |
| HUD hidden | 1280x843 | **1280x720** | 456/455 | **127 of 450** | 1 | 237/296 (307.2MB) |

Previews get four times the area, fewer fit the byte budget, and `stalls` nearly
doubles from pressing one key. **Quote `display` as well as `win WxH` for any
number taken with the HUD toggled.** `hitch` read 1 either way — the fourth time
this project has found that the threshold-independent figure is the one that
survives a changed denominator.

### Still true after phase 2

- Phase 3 should build a **shortcut table** rather than extend `keyPressEvent`'s
  flat switch, because phase 13 has to render a Keyboard Shortcuts window from
  something. Phase 2 removed two cases from that switch (`I`, `Return`/`Enter`)
  and added no new ones — the two new shortcuts live on their actions.
- Phases 4–5 must **extract the five-step shuttle sequence before adding a third
  caller**, and the buttons must enter the ladder at **2×** while J/L enter at
  **1×**.
- **There is still no text-entry control anywhere in the app**, so the spec's
  "must not fire while focus is inside a text-entry control" still has nothing to
  guard. Qt's mechanism covers it when there is one: `QLineEdit` and friends
  accept `QEvent::ShortcutOverride` for printable keys, which suppresses the
  shortcut and delivers the keystroke. **That is untested because it is untestable
  today** — Go to Frame and Go to Timecode create the first text field at phase 7,
  and verifying that `H` does not eat a digit belongs with them.

---

## Phase 3 — keyboard stepping and shuttle control contracts (2026-08-10)

### What shipped

**The flat switch in `keyPressEvent` is a `ShortcutTable`** (`src/app/ShortcutTable.*`).
`keyPressEvent` is now two lines: walk the table, fall through to the base class.
The reason is phase 13, which has to render a Keyboard Shortcuts window: a switch
works as a dispatcher and is useless as a *source*, because it cannot be
enumerated, printed, grouped, or checked for a conflict. Extending it would have
meant a second hand-written list of the same keys at phase 13 — two things to
keep in agreement, which is exactly what phase 2 spent its effort removing from
the fullscreen command.

**The table is complete and the dispatcher is not, and that separation is the
design.** Rows carrying a `QAction` (Ctrl+O, fullscreen, the HUD toggle) are
documentation only — Qt dispatched them before `keyPressEvent` was reached — and
they point *at* the action rather than copying its keys and its text, so a
changed binding cannot leave the table stale. Rows carrying a handler are the
ones this window still dispatches. `rows()` is what phase 13 renders.

**The dispatcher matches on the key and ignores modifiers**, which is precisely
what `switch (event->key())` did — Shift+Right stepped a frame and still does.
That is deliberate rather than inherited: every shortcut in Trace that carries a
modifier is *already* on a `QAction`, because `QAction` is what resolves modifier
ambiguity properly. The rule to keep is that a new modifier'd shortcut goes on an
action and this half of the table stays plain keys.

**`startShuttle()` is the five-step sequence, extracted before phases 4–5 add a
third caller.** J and L each performed `endShuttleRun` → controller ladder →
`prepareVideoRequest` → `beginPlaybackTimeline` → `startShuttleRun`, written out
twice, and the two copies had diverged in four places. §29.2 is the standing
reason this matters more than tidiness: GATE E turned playback from a
free-running timer into a timeline that must be *established*, and a path that
starts the timer without establishing it still compiles, still runs, and decays
quadratically.

**One predicate decides three things.** `ordinaryForwardPlay` — forward at exactly
1× — is simultaneously the case that keeps the play intent a scrub release
restores, the case that gets sound, and the case that does *not* become a shuttle
run. That is not a coincidence to be tidied away; 1× forward is ordinary playback
on the validated audio-mastered path and the shuttle never enters it. The HUD
shows it directly: a default L press reads `shuttle idle`, and the same press
under the 2× convention reads `shuttle RUN FWD stride 2`.

**Two of the four divergences collapsed and one did not.** J's separate
`stopAudio()` was the same thing said twice — `startAudioForPlayback()` asks
`audioShouldDrive()` first, and that already means "forward, at 1×, on a video
file with a track", so it stops the device for reverse and for every rung above
1× rather than starting it. The `clearQueue` disagreement (J true, L false) was
inert: `clearForwardQueue()` only zeroes `perfStats_.forwardQueueDepth`, a
counter for the synchronous forward-fill queue removed in July 2026 and never
written any other value.

**`landPreviousExactly` is the one the callers genuinely disagree about, and it
is preserved rather than unified.** L passes true and *must*: at L-to-1× out of a
reverse run no new shuttle run is started, so nothing else would end the old one
and its lease and queue would be stranded. J passes false: a J press always
produces a run, and that run supersedes the picture immediately. What is **not**
derivable is why L at 2× also lands — a synchronous Step decode on every forward
speed change out of reverse — when J at −2× does not. Both readings shipped in
the signed-off engine and neither has been measured against the other.
**Phases 4–5 must settle it, because the buttons are a third caller and will have
to pass something.**

**`PlaybackController` has a second documented way in** (`ShuttleEntry::AtOneX` /
`AtTwoX`). The keyboard enters the ladder at 1×; the Rewind and Fast-forward
buttons enter at 2×, which the spec states directly and the owner confirmed on
2026-08-10. The entry convention is applied at the *first rung only*, so a button
run and a keyboard run that have both reached 5× behave identically from then on.
This exists so the difference is an argument rather than a call site reaching past
the controller to write `speed`.

**`TRACE_SHUTTLE_ENTRY=2x` drives the button convention through J and L**, which
is the only way to execute it before those buttons exist. Shipping an entry point
nothing has ever run is the failure §29.2 records at length. Interim knob, like
`TRACE_VIEW_TRANSFORM` for phase 10; it leaves with the phase that makes it
redundant. Measured, first press, 4K H.264:

| first press | default | `TRACE_SHUTTLE_ENTRY=2x` |
|---|---|---|
| J | `speed -1.00x`, `shuttle RUN REV stride 1 adv 1` | `speed -2.00x`, `shuttle RUN REV stride 2 adv 2` |
| L | `speed 1.00x`, `shuttle idle` | `speed 2.00x`, `shuttle RUN FWD stride 2 adv 2` |

`starve 0` in all four. The `shuttle idle` cell is the `ordinaryForwardPlay`
predicate visible from outside: default L is ordinary playback and never becomes
a run at all.

### A real bug, found by enumerating the entry points

**The frame-step BUTTON never ended a shuttle run.** `revtransitions.ps1`
enumerates six ways out of a reverse run and every one of them is a key or the
slider. The buttons are a seventh and nothing exercised them — and the two
frame-step paths, keyboard and button, were near-copies that had drifted.

Clicking Prev Frame during a reverse run left `shuttleRunActive_` true and
`shuttleLastPresented_` holding the *shuttle's* frame. The step itself looks
fine, because the run cannot present anything once `stepBackward()` has paused the
controller and the timer is stopped — so the fault is invisible until the next
thing that ends a run takes its landing branch. Press **K** after that click and
`setCurrentFrame(shuttleLastPresented_)` puts the playhead back where the shuttle
was, **discarding the frame the user stepped to**.

Measured on a control built from `cbf6d98`, 4K H.264 — reverse, click Prev Frame,
settle, then K:

| | picture moved on K |
|---|---|
| control (`cbf6d98`) | **17.6%** — the step was discarded |
| phase 3 | **0%** |

**The obvious gesture does not find it.** Reverse → click Prev Frame → arrow-key
passes *identically* on both builds (`0%` still, then `21.3–21.6%` on the step):
it does not hang and it does not freeze, so every check written for a stranded
lease reports healthy. The gesture that exposes it is the one that ends a run
*afterwards*.

Both frame-step paths are one command now (`stepOneFrame`), reached by the arrow
keys and by the buttons through the same two `QAction`s — so when phases 4–5
re-point those buttons to Rewind and Fast-forward, the exact-frame-step command
survives untouched, which is what the spec's "do not delete the underlying
exact-frame-step commands" requires.

One more difference between the copies was resolved in the button's favour: on a
failed decode the button path reverted the playhead **and re-landed the frame it
names**, while the keyboard path only reverted the counter. Both do the former now.

### Regression

Control binary built from `cbf6d98`; both measured on the **physical panel**
(5120×1440 @ 239.999Hz, confirmed with `refresh.ps1`), `d3d11` default,
`win 1280x843`, `display 640x360`.

| run | control | phase 3 |
|---|---|---|
| 4K H.264 cadence | 99.9%, 120 frames, `handler>budget 0 of 119` (max 3.9), p50 41.9 / p99 43.5 / max 43.6, `long-gap --` | 100.0%, 120 frames, `0 of 119` (max 3.8), p50 41.8 / p99 43.5 / max 44.3, `long-gap --` |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, full-res planar, `hitch 0`, `stalls 102 of 113` | `target 120 shown 120 delta 0`, full-res planar, `hitch 0`, `stalls 103 of 114` |
| reverse 1× (`revplay`) | 20.30/24.000 fps, 99 frames, `0 of 98` (max 5.5), `hitch 0`, `starve 0` | 20.30/24.000 fps, 99 frames, `0 of 98` (max 6.4), `hitch 0`, `starve 0` |
| forward 2× (`revplay -Forward`) | 24.02/24.000 fps, 55 frames, `0 of 54` (max 2.3), `hitch 0`, `stride 2 adv 2` | 24.02/24.000 fps, 55 frames, `0 of 54` (max 2.2), `hitch 0`, `stride 2 adv 2` |
| `revtransitions -All` | six of six PASS | six of six PASS |
| lifecycle | `-PlayThroughDrag` PASS 40.3%, `-PausedThroughDrag` PASS 0%, `-PlayAfter` PASS | PASS 40.1%, PASS 0%, PASS |
| `revplay -StepCheck` | — | `+1 moved 5.6%`, `-1 returned to 0.1%` — **both legs read** |
| the seventh exit | **FAIL, 17.6% moved on K** | **PASS, 0%** |

Cadence buckets are identical (`~1x 119`, every other bucket 0). Presented frame
counts and elapsed times match to the digit on both shuttle runs.

**Every key in the table was exercised one at a time**, which is the point of
having a table: Space play/pause, Right/Left stepping (`Right then Left returns`
reads **0%** — exact), Right during playback stopping it, J/K/L, F/S/T, M, and H.
All PASS.

### What phase 3 changes about the plan

**`revtransitions.ps1` covers six exits and there are at least seven.** The
missing one was the frame-step button, and it held a real bug for as long as it
went unlisted. When phases 4–5 turn those buttons into Rewind and Fast-forward
they become shuttle *entries* as well as exits, so the enumeration needs
re-deriving then rather than extending.

**Two harness faults cost more time than the code did, and both are already
written down in this repo.** A helper named `Diff` is never called — `diff` is a
built-in alias for `Compare-Object` and aliases outrank functions — and every
check in the first smoke run reported FAIL for that reason alone. And a
picture-difference check placed at frame 0 of `Splash_1.mp4` reads 0% because the
first frames are static; stepping has to be measured mid-clip. Neither was an app
fault and both looked exactly like one.

**`revplay -StepCheck` must be given a hold short enough to stop mid-clip.** At
the default 8s, reverse traverses the 121-frame clip to frame 0, the `+1` control
leg reads 0.2% and the script says so — `stepcheck INCONCLUSIVE`. The `-1` result
is meaningless without it, which is why both legs are printed.

### Still true after phase 3

- Phases 4–5 must settle `landPreviousExactly`, and must decide what the buttons
  pass. `startShuttle(direction, entry, landPreviousExactly)` and
  `ShuttleEntry::AtTwoX` are in place, so both are call sites.
- **There is still no text-entry control anywhere in the app**, so the spec's
  "must not fire while focus is inside a text-entry control" still has nothing to
  guard. The table's key-only matching makes this *more* important to get right
  at phase 7, not less: `Go to Frame` is the first text field, and the check that
  `H` and the digits do not fight belongs with it.

---

## Phase 4 — the forward shuttle interface (2026-08-10)

### What shipped

**The visible forward control is Fast-forward.** `nextFrameBtn_` is
`fastForwardBtn_`, carrying `transport_scan_forward` and a new
`fastForwardClicked()` signal, and it triggers a new `fastForwardAction_` that
calls `startShuttle(1, ShuttleEntry::AtTwoX)`. The composited overlay's right
region moved with it: same action, same glyph.

**`nextFrameAction_` survives untouched.** The spec removes the *button*, not the
command — "do not delete the underlying exact-frame-step commands" — and it
survives without care being taken, because phase 3 had already made the button
and the arrow key trigger one action instead of two near-copies. The Right arrow
is now its only surface. A separate action rather than a re-pointed one: both
exist and they do entirely different things.

**The buttons enter the ladder at 2× and the keyboard still enters at 1×.**
`ShuttleEntry::AtTwoX` was built at phase 3 for exactly this, so the difference
is an argument to the one rate machine rather than a call site writing `speed`.
Measured on the button, from paused: **+2× → +5× → +10× → +30×**, and six rapid
presses end on `stride 30`, not back at 2. `TRACE_SHUTTLE_ENTRY=2x` still drives
J until phase 5.

**The artwork left with the behaviour, one glyph at a time.** `next-frame` is
deleted from `assets/interface/transport/` and from the `.qrc`, because this is
the commit in which the forward button stopped stepping. `prev-frame` stays, and
so does the asymmetry in `OverlayHooks` — `stepBack` beside `fastForward` — until
phase 5. That reads like an oversight and is the rule working: the two glyphs
arrived together and leave separately because their buttons change separately.

**The spec's temporary rate indicator** is a fixed-width label in the transport
bar, set on a shuttle press and cleared 1200ms later. Driven from `startShuttle`,
which is the one place a shuttle rate is ever chosen, so every surface gets it
from one call; and gated on the same `ordinaryForwardPlay` predicate that decides
whether there is a run at all, so 1× forward announces nothing. Fixed width held
whether or not there is text: a label that collapsed when it cleared would move
the slider under the pointer at the end of every run.

### `landPreviousExactly` is settled, and both halves of its justification failed

Phase 3 preserved it rather than unifying it and left the decision here. **No
shuttle press lands the previous run.** The parameter is gone from
`startShuttle`; K, Space and running off the end of the media still pass true.

**"L must pass true or the old run's lease and queue would strand" was never
about this flag.** `endShuttleRun()` calls `reclaimDecoder()` and clears the queue
*above* its `landExactly` branch, and `startShuttle` calls it unconditionally. The
lease comes back for every value of the flag. Reading the function was enough to
find this, and it went unread for a phase.

**"J passes false because a forward run supersedes the picture immediately"
described a mechanism `dd21fe9` had already removed.** Before it, an off-speed
forward run presented one frame per tick synchronously and its picture really was
already exact. It is a queued, strided run now — the same shape as reverse.

What was left was **anchoring**: the landing is a synchronous Step decode, and a
forward run decodes forward from wherever the decoder is. That is a question
about the decoder rather than about the picture, so it was measured —
`scripts/measure/shuttleland.ps1`, reverse then L, landing on against off:

| | land on | land off |
|---|---|---|
| 4K H.264 −1× → +2× | **0.8ms**, 48 frames, `starve 0`, 100.2%, handler max 2.3 | 48 frames, `starve 0`, 100.1%, handler max 2.3 |
| 4K H.264 −1× → +1× | **0.7ms**, 47 frames, 95.9%, `handler>budget 1` (max 105.1) | 47 frames, 95.9%, `handler>budget 1` (max 105.8) |
| 1080p 412f −10× → +2× | **0.3ms**, 48 frames, `starve 0`, 100.8%, handler max 0.6 | 48 frames, `starve 0`, 100.0%, handler max 0.6 |
| ProRes 4444 −1× → +2× | **25.2ms**, 46 frames, `starve 4`, 92.8% | 45 frames, `starve 5`, 90.8% |

**It buys nothing, and the mechanism says why.** On a long-GOP file the frame is
a **reverse-cache hit by construction** — the reverse run decoded and presented it
moments earlier — so the landing costs under a millisecond, and because a cache
hit sets the decoder's `currentFrame_` but never its `lastDecodedFrame`, it does
not move the decoder either. **There is no anchor to buy.** The −1× → +1× row is
the proof: ordinary 1× playback is the one path that decodes on the UI thread,
and its first tick pays a ~105ms walk out of the reverse position *with* the
landing exactly as it does without. On ProRes 4444 the cache cannot help
(`rev-hit 0.0%` — every frame is a keyframe, so nothing is ever walked past and
cached), the landing becomes a real **25.2ms block on the UI thread**, and it
still buys one frame's difference over a two-second run.

So the landing belongs to a **stop**, where the standing rule applies directly:
fidelity is owed to the frame the user stops on. A press that starts another run
re-decodes a frame that is replaced within one frame period.

**The instrument stays and reads 0 by construction.** The HUD's
`land N (Xms max Yms)` counts landings and their UI-thread cost. It has to be its
own field because the landing runs *before* `beginPlaybackTimeline()` resets the
run counters, so every other instrument in the HUD measures it as free — which is
why it had never been measured. It reads `land 0` through any press and non-zero
only on a stop, so a regression back to press-landing is visible rather than
silent. `TRACE_SHUTTLE_LAND` was the A/B knob and is **not retained**: the
measurement and the script are written down, and keeping the knob would have
meant keeping the parameter that the answer removes.

### The transition enumeration is re-derived, not extended

`revtransitions.ps1` is replaced by **`transitions.ps1`**. Its axis was six ways
*out of a reverse run*, and every one of them was a key or the slider — which is
exactly how the frame-step button went unlisted and held a real bug. At phase 4
the forward button is a shuttle **entry**, so "exits" is no longer the right axis
at all: a press that starts a run ends the previous one in the same call.

The axis is a **run boundary** — anything that starts a run, ends one, or changes
its direction or speed — from each state a run can be in. 21 cases, all PASS on
`M&M_TopGun_1080.mp4`; the nine `R` cases also all PASS on 4K H.264:

| from | commands |
|---|---|
| R (reverse run) | Space, K, Right, prevBtn, **ffBtn**, L, **J**, scrub, quit |
| F (forward run) | Space, K, Right, prevBtn, **ffBtn**, J, scrub, quit |
| P (paused), N (playing 1×) | **ffBtn** |
| delayed | R → prevBtn → K, **F → prevBtn → K** |

`R → J` and `F → J` were never enumerated before: a same-direction rung change is
a full run boundary. The whole `F` row is new, because there was no forward run
to leave. `F → prevBtn → K` is the untested mirror of the gesture that found the
phase 3 bug.

**Two harness faults, both of which produced passes that meant nothing.** The
picture signature samples a grid across the window, so **a 9:16 clip pillarboxes
four fifths of the grid onto black**: run on the 412-frame 9×16 clip the moving
cases read 13–15% and one step read 0.0%, while the same 21 cases on a 16:9 clip
read **48–49% moving** with every step between 1.5% and 50%. Both runs PASSED.
And **121 frames is too short**: at +2× the forward run reaches the tail inside
the observation window and reports `moved 0%`, a FAIL for a transition that
worked. The clip is part of the measurement.

The control locator earns its own note. Deriving button positions from the groove
by arithmetic is possible and was **wrong by ten pixels**, because QSlider insets
its groove by the handle radius; the scan finds icon pixels and asserts it found
exactly three controls, which is a thing a formula cannot do. It stops 24px short
of the groove because at frame 0 the slider **handle** is a fourth white cluster
on that exact row.

### The overlay harness was aiming 16px low, and had been for a phase

Fixing it was not optional: shipping a re-pointed overlay hook that has never
been executed is §29.2 exactly.

`overlay.ps1` predicted the panel from `0.485 × window height`. That fraction is
the bottom of the video surface, which moves whenever the HUD gains or loses a
line. By this phase it was 17px stale, putting every click **1.2px below the icon
rect**: the captures still looked right, the panel-mean still printed, and **not
one interaction leg registered**. The panel is located by *difference* now — it is
what changes between the hidden capture and the revealed one — and its size is
asserted. Found: `panel 459x75, icons y=330` against the constant's 346.

**That changes what the phase 2 overlay number was.** With nothing registering,
all twelve captures were the same paused frame, so all twelve read the same
0.619% — which is the **video band's own backend difference**, not an
overlay-agreement figure. With the legs live, 05/06/07 differ by 18–49% at max
delta 249 because each backend is on a different *frame* (`frame 116` against
`frame 120`), and the states worth comparing are the deterministic ones:

| state | cpu vs d3d11 |
|---|---|
| 01–04, paused at frame 40 | 312–317 px (0.619–0.629%), max delta 24 — the phase 2 figure, to the pixel |
| **08-mid-drag**, panel and dragged handle on screen | **0 px (0%), max delta 1** |
| 09–12, after the drag lands | 35–37 px (0.069–0.073%), max delta 7–8 |

`08-mid-drag` is the better overlay-agreement measurement, and it is exact.
`overlay.ps1` takes `-Renderer` now; it hard-coded `d3d11`, so producing the cpu
half meant editing the script, which is how a check stops being run.

### Regression

Control binary built from `0e9e5da`; both on the **physical panel** (5120×1440 @
239.999Hz, confirmed with `refresh.ps1`), `d3d11` default. `win 1280x829` /
`display 640x360` for cadence, `win 1280x843` / `display 640x360` for the drag
and shuttle runs.

| run | control | phase 4 |
|---|---|---|
| 4K H.264 cadence | 99.9%, 120 frames, `handler>budget 0 of 119` (max 3.7), p50 41.8 / p99 43.3 / max 43.4, `hitch 0` | 100.0%, 120 frames, `0 of 119` (max 4.1), p50 41.9 / p99 43.4 / max 43.7, `hitch 0` |
| 4444 cadence | 99.7%, 199 frames, `0 of 198` (max 35.5), p50 41.7 / p99 43.8 / max 44.2, `hitch 0` | 99.7%, 199 frames, `0 of 198` (max 36.4), p50 41.6 / p99 44.1 / max 45.1, `hitch 0` |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, full-res planar 1:1, `hitch 0`, `stalls 104 of 113` | same, `stalls 103 of 114` |
| forward 2× (`revplay -Forward`) | 24.01 fps, 55 frames, `0 of 54` (max 3.0), `hitch 0` | 24.00 fps, 55 frames, `0 of 54` (max 2.4), `hitch 0` |
| reverse 1× ×3 | **100.0 / 72.5 / 88.1%** | **88.1 / 88.1 / 100.0%** |
| lifecycle | `-PlayThroughDrag` PASS 40.1%, `-PausedThroughDrag` PASS 0% | PASS 40.1%, PASS 0% |
| transitions | six reverse cases PASS at phase 3 | **21 of 21 PASS** |

Cadence buckets identical on both files. Landing exact on every run.

**Reverse 1× is bimodal on this gesture, on both binaries, and the control
produced the worst run of the six.** The first pass showed phase 4 at 88.1%
against a control at 99.9% and that looked like a regression; three runs each put
both inside the same two populations — `frames 114 / elapsed 4.75s` at 100%, and
`frames 97 / elapsed 4.59s` at 88.1%. The control's 72.5% run is the interesting
one: it read **`SNAP gop 2`**, i.e. the keyframe grid was learned as 2 and
snapping engaged at stride 1. That is a pre-existing loose end and is not phase
4's; it is recorded here because a single run of this gesture cannot support a
regression claim in either direction.

### What phase 4 changes about the plan

**Phase 5 is now a narrow mirror.** `prevFrameBtn_` → `rewindBtn_`, the `rewind`
glyph, a `rewindAction_` on `startShuttle(-1, AtTwoX)`, `OverlayHooks::stepBack`
→ `rewind`, `prev-frame` leaves the tree, and `TRACE_SHUTTLE_ENTRY` leaves with
it. `transitions.ps1` needs the reverse entry added to `-Entries`, and its
`prevBtn` cases re-derived rather than kept: the backward button stops being a
step and becomes a second shuttle entry, so `R -> prevBtn` and `F -> prevBtn`
change meaning.

**Fast-forward at the end of the file does not rewind and restart.** Play does
(`c3335ec`), because Play owns the rewind; a Fast-forward press at the tail
starts a run with no target and the tick ends it, leaving `playbackAtEnd_` set so
the next Play restarts. Coherent, and worth stating because the spec does not
cover it.

**Still no text-entry control anywhere in the app**, so the spec's "must not fire
while focus is inside a text-entry control" still has nothing to guard. Phase 7
creates the first one.

---

## Phase 5 — the reverse shuttle interface (2026-08-10)

### What shipped

**The visible backward control is Rewind, and the transport now has no frame-step
button at all.** `prevFrameBtn_` is `rewindBtn_`, carrying `transport_scan_reverse`
and a new `rewindClicked()` signal, and it triggers a new `rewindAction_` calling
`startShuttle(-1, ShuttleEntry::AtTwoX)`. The composited overlay's left region
moved with it: same action, same glyph. `OverlayHooks::stepBack` is `rewind`, so
the asymmetry phase 4 deliberately left behind — `stepBack` beside `fastForward` —
disappears in the commit that earned the right to remove it.

**`prevFrameAction_` survives untouched**, with the Left arrow as its only surface,
exactly as `nextFrameAction_` did at phase 4. The spec's "frame stepping becomes
keyboard-only" is now literally true rather than half true.

**`TRACE_SHUTTLE_ENTRY` is gone.** It existed for one reason — running the buttons'
2× entry before the buttons existed, because shipping an entry point nothing has
ever executed is §29.2 — and both buttons now pass `AtTwoX` as an argument. J and L
name `ShuttleEntry::AtOneX` literally at their call sites. The knob leaves with the
phase that made it redundant, as `TRACE_VIEW_TRANSFORM` will at phase 10.

**`prev-frame-{24,48,72}.png` and `prev-frame.svg` left `assets/interface/transport/`
and the `.qrc` in the same commit that stopped the button stepping.** That directory
is now exactly the approved package's glyphs and nothing else, and nothing in the
tree has a `-72` rendition any more — the two frame-step glyphs were the only ones
that did, which was a property of the superseded first-pass set rather than of the
controls. `loadIcon`'s `-72` branch is kept as insurance rather than removed; it is
one line and it is what makes "is the 3x file being used" answerable by reading.

Both ladders, driven from the button and read off the HUD:

| control | press 1 | 2 | 3 | 4 | 6 rapid |
|---|---|---|---|---|---|
| Fast-forward | +2× | +5× | +10× | +30× | **+30×** |
| Rewind | −2× | −5× | −10× | −30× | **−30×** |

### The transition axis is re-derived for the third time, and the negative control is the point

`transitions.ps1`'s phase 4 axis was a run boundary from each state a run can be
in. Phase 5 did not add rows to it; it changed what two of them **mean**, and the
distinction matters because the old names would have kept passing:

- **`R -> prevBtn` and `F -> prevBtn` became `R -> rewBtn` and `F -> rewBtn`, and
  their expectation flipped from `still` to `moving`.** They were "step out of a
  run"; they are a same-direction rung change and a direction change now. Left in
  place, they would have asserted that pressing Rewind stops playback.
- **`R -> Left` and `F -> Left` are where the old coverage went**, not new cases.
  A backward step out of a run still has to be exercised, and after this phase the
  arrow key is the only control that performs one.
- **`-Delayed` was re-pointed rather than deleted.** Its gesture was "step with a
  BUTTON during a run, then press K" and there is no such button — but the button
  was never the point. The point is that a step leaves run state behind which only
  the *next* run-ending command exposes, and the arrow key inherits that shape
  exactly. `R -> Left -> K` and `F -> Left -> K` are expected to pass on every
  build, because the phase 3 fault was in the button copy; they are kept because
  nothing else in the matrix has that shape.

**25 of 25 PASS on phase 5.** Moving cases read 46.8–49.7%, still cases 0%, and
every step moved between 1.5% and 29.4%.

**The control run is what makes the re-derivation mean anything.** Run against a
binary built from `e559d07`, the matrix reports **exactly four FAILs and they are
exactly the four `rewBtn` cases** (`FAIL (frozen, expected moving) - moved 0%`),
with all 21 other cases reading the same as on phase 5. A harness that passes on
both builds is not testing the change.

### The ladder cap leg could not pass on any build, and had been reporting a wrapped ladder

Found while adding the reverse ladder. The cap leg presses six times without
capturing and then captures once, to prove the sixth press reads 30× rather than
the first rung. It read **`speed 2.00x` at `frame 406` of 412** — which is the
exact appearance of a ladder that wrapped, and is instead the run having ended.

The arithmetic was never survivable. At 30× a 412-frame 24fps clip is traversed in
**0.57s of wall time**, and `Click` spends ~210ms of dwell *per press* before the
loop's own spacing, so six presses spanned ~1.6s. The leg spent three times its
whole budget on mouse timing. `FastClick` (45ms) and no settle before the capture
bring the six presses to roughly 150 frames of the 412, and both legs then read
what they exist to assert: **`speed 30.00x`** forward and **`speed -30.00x`**
reverse, both still running.

Third instance in two phases of a harness that could not have failed — after the
9:16 signature and the overlay aim — and the first where the fault made a correct
build look broken rather than a broken one look fine.

### The overlay's left hook is executed, not merely wired

Phase 4's lesson applied directly: `overlay.ps1`'s state 07 was "step one frame
back" and is a **reverse shuttle press** now. Read off the HUD at that state:

- **d3d11 07**: `Play <` · `frame 96` · **`speed -2.00x`** · `Reverse Play`
- **cpu 07**: `Play <` · `frame 92` · **`speed -2.00x`** · `Reverse Play`
- d3d11 06 (two forward rungs): `Paused` · `frame 118` · `speed 0.00x` · `FF`

`-2.00x` is the button's `AtTwoX` entry arriving through the overlay, on the path
that ships and on the escape hatch alike. 06 reads paused because two forward rungs
from frame 40 reach the tail of a 121-frame clip — the run ended, and the `FF`
label is what says the hook fired.

Cross-backend agreement is unchanged and reproduces phase 4 to the pixel:

| state | cpu vs d3d11 |
|---|---|
| 01–04, paused | 312–317 px (0.619–0.629%), max delta 24 |
| **08-mid-drag** | **0 px (0%), max delta 1** |
| 09–12 | 35–37 px (0.069–0.073%), max delta 7–8 |

One reading moved: `03-hover-play` kept its 314 px but its max delta read 246
rather than 24. Same pixel count, one outlier — the hover fill is animation-timed
and the two captures are not guaranteed to be at the same point in it. Recorded
rather than chased.

### Regression

**This session ran on a 1920x1080 @ 59.999Hz display, NOT the physical panel**
(`refresh.ps1`; the panel is 5120x1440 @ 239.999Hz). So none of the figures below
are comparable to the phase 2–4 tables, and the control was rebuilt and measured
here rather than compared across displays. `stalls` and `hitch` coincide at this
refresh, because the stall bar is `2 × refresh` = 33.3ms. **No subjective judgement
was taken and none is valid from this display.**

Control binary built from `e559d07`; `d3d11` default; `win 1280x843`,
`display 640x360`.

| run | control | phase 5 |
|---|---|---|
| 4K H.264 cadence ×3 | 100.0% all three, 120 frames, `handler>budget 0 of 119` (max 3.8/3.6/4.0), max 44.2/44.0/43.9 | 100.0% all three, 120 frames, `0 of 119` (max 3.6/3.8/3.6), max 43.6/43.6/43.3 |
| 4444 cadence | 99.8%, 247 frames, `0 of 246` (max 35.9), p50 41.6 max 44.4 | 99.8%, 247 frames, `0 of 246` (max 32.9), p50 41.7 max 44.0 |
| `scrub -SnapRelease` | `target 2 shown 2 delta 0`, `walk 0f`, `rev-hit 98.4%`, `hitch 1`, `stalls 1 of 441`, `ui gap max 73.6ms` | same landing, `rev-hit 98.4%`, `hitch 1`, `stalls 1 of 443`, `ui gap max 73.9ms` |
| reverse 1× ×3 | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 5.5/6.4/5.8) | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 5.7/4.9/4.7) |
| forward 2× | 100.2%, 55 frames, `0 of 54` (max 2.3), `hitch 0`, `land 1 (1.0ms)` | 100.2%, 55 frames, `0 of 54` (max 2.0), `hitch 0`, `land 1 (1.0ms)` |
| lifecycle | `-PlayThroughDrag` PASS 40.3%, `-PausedThroughDrag` PASS 0% | PASS 39.1%, PASS 0% |
| transitions | **4 FAIL — the four `rewBtn` cases — 21 PASS** | **25 of 25 PASS** |

Cadence buckets identical on both files (`~1x 119`, every other bucket 0 on 4K
H.264). Landing exact on every run. `land` reads **0 through every shuttle press**
and 1 only where a run ended by reaching the tail, which is a stop — the phase 4
decision is intact and still visible.

**One outlier, not reproduced, recorded because the first run is the one that would
have been quoted.** The very first phase 5 cadence run — the first after a binary
swap — read `max 74.1ms`, `jitter max 32.39`, and one frame each in the `<0.9x` and
`>2.5x` buckets. Three subsequent runs of the same file on the same binary read
`~1x 119` with every other bucket 0, and `handler>budget 0 (max 3.5)` on the
outlier itself says the decoder was never the cause. Same discipline the bimodal
reverse gesture forced at phase 4: one run cannot support a claim in either
direction.

**Reverse 1× did not go bimodal at all this session.** All six runs — three per
binary — read 100.0% at `frames 114 / elapsed 4.75s`, and none reported
`SNAP gop 2`. That is not evidence the loose end is fixed; it is a different
display from the one it was seen on, and it stays open and unattributed.

### What phase 5 changes about the plan

**The transport redesign is complete, and the spec's validation list for it reads
straight down**: first frame is 0, Right and Left step exactly one frame, no
on-screen frame-step buttons remain, both ladders run 2→5→10→30 and cap there, an
opposite-direction press starts at 2× in the new direction, Play restores 1×, Pause
clears the rate, and the rate indicator shows each rung.

**Phase 6 is the one most likely to cost performance** — removing `transportBar_`
from the layout in favour of the floating overlay — and it now has one more thing
riding on it: after it, the *only* Rewind and Fast-forward controls that exist are
the overlay's. Its open question is plan §31.5 item 2, whether the overlay's
timeline press lands exactly the way a groove click does. The overlay's input path
has now been exercised properly twice, which is still new rather than established.

**One weakness left in `transitions.ps1`, stated rather than fixed.** The
post-transition step is asserted only as `$stepped -lt 0` — an unresponsive app —
so a step that moves the picture by **0%** passes. One control case (`F -> J`) did
exactly that, where phase 5 read 2.1% on the same case. Raising it to a floor needs
the floor established across all 25 cases first, and guessing one would make the
matrix flaky in exchange for catching something no build has yet done.

**Still no text-entry control anywhere in the app.** Phase 7 creates the first one.

---

## Phase 6 — fullscreen consolidation and overlay auto-hide (2026-08-11)

### What shipped

**The floating transport is the transport.** `transportBar_` comes out of the
`QVBoxLayout` and the composited overlay is on by default.
`OverlayModel::enabledByEnvironment()` decides it once for the whole application
— `MainWindow` asks it whether to dock the bar, `ViewerWidget` asks it whether to
draw the overlay — so the two cannot disagree and **there is no combination of
knobs that leaves the window with no transport at all**. `TRACE_OVERLAY=0` and
`TRACE_OVERLAY_COMPOSITED=0` now select the bar rather than selecting nothing.

**`TRACE_TRANSPORT_BAR=1` restores the docked bar, and it is not only an escape
hatch.** Eight measurement scripts locate the timeline by scanning for its groove
colour; without a way back to the bar, most of the regression suite would have
had to be rewritten in the same commit as the feature it exists to check. It is
also the negative control every figure below is measured against.

**The bar OBJECT stays alive in both modes, and that is deliberate.**
`timelineSlider_` is its child and is the whole scrub state machine — the
press/move/release the overlay drives, `isSliderDown()`, `scrubJumpPending_`, the
step 5.6 play-intent restore. Re-homing that into the overlay would mean a second
scrub path, which is the one thing the `OverlayHooks` design exists to avoid. So
the overlay drives the real slider and the slider simply is not on screen.

**The approved package's control geometry is adopted**: 44×44 play/pause, 34×34
utility targets, in a 460×84 rounded panel. Phase 2 declined to apply it to the
bar precisely because this is the geometry of the floating transport. Two icon
sizes mean two snapped sizes in `layout()`, both reused by `rebuildAtlas()` — the
property that keeps each atlas cell exactly the size of the rect it lands in, and
therefore keeps the two backends agreeing pixel for pixel.

**Auto-hide is finished to the spec's list.** Pointer movement, a click anywhere
over the video, and any key the `ShortcutTable` dispatches all reveal it; 2s of
inactivity fades it. The overlay checks the two hold conditions it can see
(pointer over a control, timeline drag in progress) and **asks the host for the
rest** through a new `holdVisible` hook — a popup menu, a tooltip, a modal dialog
or a child control holding focus are application state a renderer cannot see.
**The keyboard reveal is defined by the table rather than by a key list**: every
row in it is a transport, stepping, shuttle or readout command, so a key the
table does not own is by construction not "relevant keyboard input".

**The cursor hides in fullscreen on the same idle timer**, through a
`setCursorHidden` hook. The overlay decides *when*; the host decides *whether*,
because the spec asks for it in fullscreen only. Both backends had to implement
it and they do it differently — see the measurement note below.

**Fullscreen is consolidated.** Escape exits, geometry is preserved and restored,
maximize stays distinct from fullscreen, and double-clicking the video toggles
it. `nextFrameAction_`-style, Escape is a **second surface onto
`fullscreenAction_`, not a second definition**: `exitFullscreenAction_` triggers
it and owns no window state. It is a separate action rather than a fourth
shortcut because **a disabled QAction does not consume its shortcut** — so
"Escape means this only while fullscreen" is expressed as enablement rather than
as a branch inside a handler that has already swallowed the key. It also could
not live in `ShortcutTable`'s plain-key half, whose dispatcher consumes
unconditionally.

**One rate string, two surfaces.** `hooks.rateText` was reading
`playback_.state().speed` directly and was therefore permanently non-empty — the
overlay showed "1.0x" or "PAUSED" whatever was happening, which is a HUD, not a
transport. It reads `MainWindow::rateFlashText_` now: the same transient the
docked bar's label shows, driven from `startShuttle` and cleared by the same
1200ms `TransportBar::rateFlashMs()`.

**A backend that cannot draw the overlay is now a fallback condition.**
`VideoRenderer::overlayDrawFailed()`, read by `adoptRenderer`. It was survivable
while the overlay was an off-by-default spike; with the overlay as the only
transport it would leave the window with a picture and no controls.

### The double-click found a real bug, and it was found by accident

While setting up an unrelated check, two deliberate single clicks 300ms apart
landed on the video and the window went fullscreen. Correct — and it exposed the
case one step to the left.

**Windows sends down, up, DBLCLK, up.** The second press of any pair inside the
double-click interval arrives as `WM_LBUTTONDBLCLK` and **not** as
`WM_LBUTTONDOWN`. The first cut of `onMouseDoubleClick` consumed it when the
point was over a control, on the reasoning that "the press half already ran the
command". It had not: that press *was* the double-click. So **every other rapid
press on an overlay control was dropped** — and after phase 6 the overlay's is
the only Fast-forward there is, which makes the spec's own ladder gesture (six
rapid presses must reach 30×) the thing that breaks.

**The docked bar never had this problem**, which is exactly why it would have
gone unnoticed: `QWidget::mouseDoubleClickEvent` forwards to `mousePressEvent`,
so Qt's buttons already did the right thing. The overlay reads raw Win32 messages
on the D3D11 path and had to be taught it. The fix is one line — forward to
`onMouseDown` — and it mirrors what Qt does.

`scripts/measure/overlay_ladder.ps1` is the check, and it was run against a
binary with the fix reverted:

| six rapid presses | fixed | pre-fix (negative control) |
|---|---|---|
| Fast-forward | **`speed 30.00x`**, frame 90, still running | `speed 10.00x`, frame 49 |
| Rewind | **`speed -30.00x`**, frame 354, still running | `speed -10.00x`, frame 382 |

10× is three rungs of six presses — one press per pair, lost.

**And the leg could not pass twice before it could fail once**, which is the third
instance of that in three phases. `restart.ps1` leaves the playhead at frame 0, so
the rewind leg ran off the head of the clip immediately and read `speed 0.00x`;
it needs a positioning click first (`-StartFraction`, 0.95 backward / 0.05
forward). Then `capture.ps1` raises the window and sleeps **300ms**, which at 30×
on a 24fps clip is **216 frames** — enough on its own to run a 412-frame clip off
the end and capture an ended run. The script grabs the window directly instead.
Phase 5 hit the same arithmetic from the other side.

### Does the overlay's timeline press land exactly? Yes — plan §31.5 item 2 is closed

The open question was that a groove click is an absolute set (value first, then
`sliderPressed`) while the overlay calls `setSliderDown(true)` then `setValue()`
— the opposite order — and §31.5 required testing it **with the playhead
deliberately far from the press point**, because a press already near the target
cannot tell an exact landing from a lazy one.

`scripts/measure/overlay_press.ps1`, 4K H.264, playhead at frame 0, single click
at 0.85 of the track:

| | overlay track | groove control |
|---|---|---|
| landed | `frame 101`, `target 101 shown 101` | `frame 102`, `target 102 shown 102` |
| `delta` | **0** | **0** |
| `dst` | `YUV420P8 planar` (full-res) | `YUV420P8 planar` (full-res) |
| walk / seeks | `walk 11f`, `n=2` | `walk 12f`, `n=2` |

Both take one seek plus a GOP walk and both land full-resolution and exact. The
one-frame difference is pixel quantisation — the overlay's track is 404px against
the groove's 827px — not a landing error.

### Regression

**This session ran on a 1920x1080 @ 59.999Hz display, NOT the physical panel**
(`refresh.ps1`; the panel is 5120x1440 @ 239.999Hz), the same display phase 5 ran
on. `stalls` and `hitch` coincide at this refresh. **No subjective judgement was
taken and none is valid from this display** — the overlay's feel is the owner's
and has to be taken at the machine.

Control binary built from `fec93f0` in a separate worktree and **verified by
hash** on every swap. `d3d11` default throughout.

**Bar mode — phase 6 against the control, layouts identical, `win 1280x843`,
`display 640x360 filtered x3`:**

| run | control | phase 6 (`+bar`) |
|---|---|---|
| 4K H.264 cadence ×3 | 99.1 / 99.1 / 99.1%, 120 frames, `handler>budget 0 of 120` (max 3.6), max 84.3 / 84.0 / 83.7 | 99.2 / 99.1 / 99.2%, 120 frames, `0 of 120` (max 3.9 / 3.5 / 3.7), max 84.2 / 84.8 / 84.6 |
| 4444 cadence ×2 | 99.8 / 99.7%, 261 frames, `0 of 260` (max 34.0 / 34.1), max 45.3 / 46.5 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 34.8 / 32.9), max 44.4 / 44.7 |
| reverse 1× ×3 | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 5.0 / 5.5 / 5.2) | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 3.3 / 3.1 / 3.3) |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, `walk 0f`, `hitch 0`, `ui gap max 18.4ms` | same landing, `hitch 0`, `ui gap max 17.9ms` |
| lifecycle | `-PlayThroughDrag` PASS 40.1%, `-PausedThroughDrag` PASS 0% | PASS 39.6%, PASS 0% |
| transitions | **25 of 25 PASS** | **25 of 25 PASS**, case for case |

Cadence buckets identical on both files (`~1x 118`, one frame in `1.5-2.5x`, every
other bucket 0 on 4K H.264). **No run of the six reverse gestures reported
`SNAP gop 2`** — and as at phase 5 that is *not* evidence the loose end is fixed,
because this is not the display it was seen on.

**Overlay mode — the configuration that ships:**

| run | value |
|---|---|
| 4K H.264 cadence ×3 | 99.1 / 99.2 / 99.2%, 120 frames, identical buckets, `handler>budget 0 of 120` (max 4.0) |
| 4444 cadence ×2 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 36.3 / 36.1) |
| overlay drag ×2 vs groove control ×2 | `hitch 1` all four, `delta 0` all four, landing exact all four |
| overlay ladder | ±30.00× on six rapid presses, both directions |
| cross-backend, 12 states | `08-mid-drag` **0 px (0%), max delta 1** |

**The overlay's cost is paints, and it is visible and small.** Playback:
`paints 152/121` against `120/121` — about 32 extra across a 5s run, from the
fade and the reveal, at `paint 0.01/0.04ms` against a 41.67ms budget. During a
drag: `paints 559/469` against `440/441`, roughly 120 extra at 0.02ms each.
**4444 is the file with the least headroom** (handler ~23ms) and it absorbed the
extra paints with `handler>budget 0 of 260` and 261 frames, unchanged.

**The `ui gap max` asymmetry reproduced and is still unattributed** — 9.6 / 7.3ms
on the overlay track against 76.0 / 72.9ms on the groove. §31.2 measured 17
against 84ms and said explicitly not to quote it as an overlay win. It still is
not one.

### What phase 6 changes about the plan

**The video rect did NOT move, and the handoff predicted it would.** The
prediction was reasonable — the bar is 76 logical pixels and §22.8 says cache
depth follows the video rect — but at the default startup size the **window**
shrinks instead, because it is sized from the layout's own hint and the viewer
keeps its 640×360 minimum:

| | `win` | `display` |
|---|---|---|
| 4K H.264, bar | 1280x843 | 640x360 filtered x3 |
| 4K H.264, overlay | **1280x767** | **640x367** filtered x3 |
| 4444, bar | 1280x843 | 652x367 filtered x4 |
| 4444, overlay | **1280x760** | **652x367** filtered x4 |

So the §22.8 effect is real but is not reached by this change at the default size,
and that is *why* the stall and cache figures did not move — an explanation rather
than an observation. **At a held window size the video rect would grow**, so a
maximized or user-sized window is where to look if a scrub figure is ever
questioned; quote `display` either way.

**`stalls` could not be compared across modes on the same gesture**, and the
reason is worth keeping: `scrub.ps1` locates the groove, so in overlay mode it
exits before dragging — the capture read `paints 0/1` and `frame 0`, which is a
harness that ran nothing rather than an app that did nothing. `overlay_drag.ps1`
is the comparison that works, because it drives the overlay's own track against
the groove as its control.

**The naive cursor instrument says the CPU backend is broken and it is not.**
`GetCursorInfo` reports `flags=1` (CURSOR_SHOWING) on `cpu` even when the pointer
is hidden, because `Qt::BlankCursor` is a real cursor with an empty bitmap, while
the D3D11 surface answers `WM_SETCURSOR` with `SetCursor(nullptr)` and reads
`flags=0`. The **handle** is what separates them: on `cpu` it moves `0x10003` →
`0x6470DA7` → `0x10003` across idle and back. Two mechanisms, one behaviour, and
the obvious measurement sees only one of them.

**Fullscreen was verified against the window manager, not against the toggle:**
F11 → `0,0 1920x1080`; Escape → `133,77 1100x806`, the pre-fullscreen rectangle
exactly; a second Escape changes nothing, so the shortcut really is unclaimed
when windowed. Maximized → F11 → Escape returns **MAXIMIZED**, and a later
restore returns the pre-maximize rectangle. Double-click toggles on **both**
backends (`CS_DBLCLKS` had to be added to the surface window class or
`WM_LBUTTONDBLCLK` is never sent at all).

**`TRACE_RENDERER=cpu` keeps its transport — verified, not assumed.**
`overlay.ps1` was run on both backends and reports the same panel geometry, the
same twelve states and the same interactions on each. That was §2 item 1's whole
requirement for this phase.

**Still no text-entry control anywhere in the app.** Phase 7 creates the first
one, and `holdVisible` already carries the child-focus clause it will need — it
cannot fire today because every transport widget is `Qt::NoFocus`, and it is
written now so the omission would be deliberate rather than invisible.

### Owner sign-off — PASSED, 2026-08-11

**The visual sign-off passed and no tuning is wanted.** The floating panel
**clearly reads as the transport**, the **2s inactivity delay feels right**, and
the **165ms fade feels natural**. Verdict: proceed to phase 7.

Read that at its stated width, as with every sign-off in this project. What was
accepted is the **feel of the auto-hide and the panel's identity as a transport**
— the fade duration, the idle delay, and the read. It is not a sign-off on the
Time Display readouts (phase 7 changes them), on the menus (phase 13), or on the
overlay being finished: §31.5 item 4 still stands, and the overlay **must not be
called final until a screen reader has driven one**.

So `kFadeMs` (165), `kAutoHideMs` (2000) and the 460×84 panel with its 44×34
controls are now **settled numbers, not defaults**. Changing any of them is
reopening an owner decision rather than tuning a constant.

**Nothing about the floating transport's behaviour or feel is open.**

---

## Phase 7 — Time Display and zero-based frame UI (2026-08-11)

### What shipped

**The `Timecode:` readout was non-conforming and now is not.** It printed
`TimeFormat::frameToTimecode(currentFrame, fps)` — an elapsed-time conversion of
the frame index — under the label `Timecode:`, for every file: ignoring the real
start timecode on files that carry one, and inventing `00:00:00:00` for files
that carry none. The spec forbids both halves (*"do not generate SMPTE from zero
when none exists"*, *"do not label an elapsed-time conversion as source
timecode"*). §2 item 9 called this "worse than not extracted" and it was right.

So the readout is four modes now, and **Elapsed and Timecode are separate
things**: `frameToTimecode` is renamed **`frameToElapsed`**, which is the honest
name for what it always computed, and `Timecode` means the source's.

| key | mode | shows |
|---|---|---|
| `F` | Frame Count | zero-based index |
| `S` | Seconds | Trace's existing readout, kept because `S` is an existing binding |
| `E` | Elapsed Time | `HH:MM:SS:FF` counted from zero |
| `T` | SMPTE Timecode | the source's, and **only when the source has one** |

**`hasSourceTimecode_` is the single gate**, and the readout mode, the menu item
and Go to Timecode all ask it — so "this file has no timecode" cannot be true in
one place and false in another. `setReadoutMode` **declines** SMPTE with a
reason rather than accepting it and rendering something else; opening media
without a timecode while SMPTE is selected resets the mode to Elapsed, which is
the case the gate alone cannot catch because nothing was selected — the file
changed underneath a mode already set.

**Extraction reads three dictionaries and never synthesises.** FFmpeg's mov
demuxer publishes a tmcd track's value on the *format* dictionary, MXF and some
MOVs put it on the video stream, and a few containers leave it only on the data
stream that carries it; all three are checked. The value is **parsed and
re-formatted** rather than stored raw, so anything unreadable becomes "no
timecode" inside the decoder instead of reaching a readout that would print it
verbatim and call it SMPTE. A drop-frame claim at a rate with no drop-frame is
discarded, and a frame number the rate cannot produce rejects the whole value.

**Drop-frame arithmetic is real**, against the exact rational rather than the
double — `nominalRate`, `dropFrameApplies`, `timecodeToFrames` and
`framesToTimecode`, which round-trip. Timecode counts whole frames per second,
so 30000/1001 material runs 30 timecode frames per timecode second; that is what
drop-frame exists to reconcile and it cannot be done from a rounded rate.

**Go to Frame and Go to Timecode are the first text-entry controls in Trace.**
Both are `QInputDialog` — modal, owning their own Escape and Return, and
reported by `QApplication::activeModalWidget()`, which is what the overlay's
phase 6 `holdVisible` hook already asks. Both validate **before** seeking and
**refuse rather than clamp**: a mistyped timecode that got clamped would move the
playhead somewhere the user did not ask for and look like it had worked. Both
land through `goToFrame()`, one shared exact `Step` seek, so neither needed any
decoder work.

**Zero-based numbering is finished.** The image-sequence and still HUD lines
printed `currentFrame + 1` against a frame *count*; they print the index against
the last valid index now, which is what the video line has always done (§2 item
8). They also stopped saying `Timecode:` — an image sequence has no container
timecode at all, so that was the clearest instance of the thing the spec forbids.

### The shortcut guard finally had something to guard, and it holds

Since phase 3 the record has said, five times, that `ShortcutTable` matches on
the key and ignores modifiers, that this makes a text field dangerous, and that
it was **untestable because it was untestable** — there was nothing to type into.

Measured: with Go to Timecode open, typing `hjkltefsm` — every bound single-key
command in the app — puts **`hjkltefsm` in the field** and changes nothing behind
it. `H` did not hide the HUD, `J`/`K`/`L` did not shuttle, `T`/`E`/`F`/`S` did not
change the readout, `M` did not mute. The transport line still read
`Paused | frame 0 | speed 0.00x | Open file | Frame: 0`.

Two mechanisms do it and neither needed writing: `QLineEdit` accepts
`QEvent::ShortcutOverride` for printable keys, and a modal dialog is a separate
window whose key events never reach `MainWindow::keyPressEvent` at all. **Both
were predicted at phase 2 and neither had ever executed.**

`Ctrl+G` and `Ctrl+Shift+G` carry modifiers, so they go on `QAction`s by the rule
phase 3 set, and appear in the table as documentation rows.

### Drop-frame had no test material, so the material was made

`Trace_Testing_Assets` is 24, 23.976 and 60fps throughout; the three files that
carry a timecode carry `00:00:01:12`, `00:00:00:00` and `00:00:00:00`, all
non-drop. Shipping drop-frame arithmetic that had never executed is §29.2
exactly, so `scripts/measure/make_timecode_fixtures.ps1` generates a matched
29.97 drop / non-drop pair.

**The first pair could not have failed.** Starting at `00:59:50` and running past
the hour crosses minute 60 — a multiple of ten, where drop-frame skips nothing —
so both conventions printed identical digits and differed only in the separator.
Starting at `00:00:50` puts minute 1, a dropping minute, inside the clip:

| at the same frame index | drop-frame fixture | non-drop fixture |
|---|---|---|
| frame 0 | `00:00:50;00` | `00:00:50:00` |
| frame 300 (`Ctrl+G`) | **`00:01:00;02`** | **`00:01:00:00`** |

The `;02` is the two frame numbers minute 1 skips. That difference is the proof
the drop-frame path runs rather than compiles.

**On real production media**, ProRes 4444 with a start timecode of `00:00:01:12`:
frame 0 reads `Timecode: 00:00:01:12` and twelve Right steps read
`Timecode: 00:00:02:00` — a non-zero start honoured, and exactly one second on at
24fps.

**And the refusals were run, not argued.** On a no-timecode MP4, `T` reads
`Timecode: source carries none` and the readout stays on `Frame:`; `E` reads
`Readout: Elapsed | Elapsed: 00:00:00:00`. On the drop-frame fixture with the
playhead at frame 25, a malformed `09:99:99;99` and a well-formed but
out-of-range `00:05:00;00` both read `Go to Timecode: rejected | Frame: 25` —
unmoved in both cases.

### Regression

Control binary built from `19f9383` in a worktree, **hash-verified on every
swap**; same 1920x1080 @ 59.999Hz display as phases 5 and 6. Cadence in overlay
mode (the shipping configuration); the drag, reverse and matrix runs in bar mode
with `TRACE_TRANSPORT_BAR=1`, `win 1280x843`, `display 640x360 filtered x3`.

| run | control | phase 7 |
|---|---|---|
| 4K H.264 cadence ×3 | 99.2 / 99.1 / 99.1%, 120 frames, `handler>budget 0 of 120` (max 3.9 / 4.1 / 4.1), max 82.4 / 81.7 / 82.5 | 99.1 / 99.2 / 99.1%, 120 frames, `0 of 120` (max 4.0 / 4.1 / 4.0), max 81.7 / 82.8 / 82.9 |
| 4444 cadence ×2 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 37.5 / 36.7), max 44.9 / 45.4 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 36.1 / 36.2), max 44.7 / 45.0 |
| reverse 1× ×3 | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 3.1 / 3.3 / 3.9), `hitch 0` | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 3.5 / 3.0 / 3.6), `hitch 0` |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, `walk 0f`, `hitch 0`, `ui gap max 18.4ms` | same landing, `hitch 0`, `ui gap max 17.8ms` |
| lifecycle | `-PlayThroughDrag` PASS 40.3%, `-PausedThroughDrag` PASS 0% | PASS 39.1%, PASS 0% |
| transitions | **25 of 25 PASS** | **25 of 25 PASS** |

Cadence buckets identical on both files. Landing exact on every run. **No run of
the six reverse gestures reported `SNAP gop 2`** — six more clean runs on the
wrong display, which still is not evidence.

### What phase 7 changes about the plan

**§2 item 9 is closed and the decision it left open was taken: relabel, not
disable.** Disabling the readout would have left `T` doing nothing on most files,
which is the `showInfo` failure phase 2 deleted. `T` now either shows the
source's timecode or says the source has none, and `E` is the honest elapsed
readout that mode used to be.

**§2 item 8 is closed.** Video was already zero-based including the right
endpoint; the two image-kind display strings are now too. It stayed two strings
and did not become a conversion layer.

**The spec's "must not fire while focus is inside a text-entry control" is
answered and needs no code.** Qt's mechanism covers it, and it is now exercised
rather than predicted. **A new single-key shortcut still has to be checked against
this** — the guard is Qt's, not Trace's, and it only covers *printable* keys.

**`TRACE_OPEN_LOG` gained a `timecode=` column**, printing `none` rather than an
empty field, because absence is the answer this extraction most often gives and a
blank column in a tab-separated log reads as a broken logger.

**Phase 8 is next** — the Share menu and ordinary path copying — and §2 item 5's
distinction is the thing to carry into it: `MediaIoSource`'s classifier answers a
*storage-class* question, which is a good necessary condition for LucidLink and a
bad sufficient one. The authoritative gate is the installed integration.

---


---

## Phase 8 — the Share menu and ordinary path copying (2026-08-11)

### What shipped

**Three commands, three surfaces, one QAction each.** `copyFilePathAction_`,
`copyLucidLinkAction_` and `showInExplorerAction_` are created in
`setupSharedActions()` and reached from the menu bar (File ▸ Share), the docked
transport bar's Share button, and the composited overlay's Share region. There is
also exactly **one `QMenu` instance**, popped by both transport surfaces and
reused as the menu bar's submenu — two menus built from the same actions would
still be two things to keep in step.

**The gate is `src/app/MediaShare.*`**, a small module rather than more
`MainWindow`, because phase 9 extends exactly one function in it and because the
reasoning below is the whole content of the phase.

**Copy File Path** copies the canonical, native-separator Windows path and
confirms in the status bar — the mechanism every other confirmation in Trace
already uses, so it needed no new one. **Show in File Explorer** opens the
containing folder with the file selected.

**Copy LucidLink Link is present, visible, and cannot run.** That is the design
package's *Unavailable* state (§9) and it is deliberately not a hidden row: a
command that comes and goes reads as a broken build rather than as an answer
about the file. The action is **not connected to a handler at all**, which is the
phase boundary rather than an omission — an action that appears to exist and
changes nothing is the `showInfo` failure phase 2 deleted.

### The gate, and why the classifier can only ever say no

The spec's three conditions are file-backed, on a LucidLink filespace, and
integration available. `MediaIoSource::classifyStorage` supplies the second and is
**reused rather than rewritten** — it queries the volume, never writes a probe
file, and is cached per volume, so asking it costs one lookup per media open.

**But it answers a storage-class question, not a vendor one.** "Virtual mount
advertising petabyte capacity with `free == total`" is true of any such mount. It
is a good **necessary** condition for LucidLink and a bad **sufficient** one, so
in `evaluateShare` it can only move the verdict from Unavailable to **Disabled**,
never to Available. Treating it as sufficient would reintroduce, one level up,
exactly the "assume every `V:\` path is LucidLink" mistake the requirement exists
to prevent.

`lucidLinkIntegrationAvailable()` returns false today, with a reason that says
only what has been established — that this build has no integration — rather than
the design package's "LucidLink is not running", which asserts a cause nothing
here has checked. Phase 9 replaces the body and the string together. It is a
named function rather than a `false` at the call site so the gate reads as three
conditions now and still reads as three when one of them starts returning true.

### The Share button fits inside the settled panel

`kPanelWidthLogical`, `kPanelHeightLogical` and the 44/34 control sizes became
**owner-signed-off numbers at phase 6**, so changing one reopens a decision
rather than tuning a constant. It did not need changing: the three centred
controls only reach 78 logical px either side of centre, so the right end of the
row was already empty — and that is where the approved package puts share
("rewind · play/pause · forward | fullscreen · share").

**One thing did have to move, and it was a real collision rather than a
preference.** The rate-flash chip sat at the panel's **top-right**. At 84px of
panel height the chip spans y 10–31 and a 34px control centred on the row spans
13–47, so they overlap outright. The chip is **top-left** now — the smallest
change that resolves it, with the panel, the controls, the fade and the auto-hide
all untouched. Worth recording that the approved package actually specifies the
rate chip **centred above the transport** rather than inside it (§6, with its own
padding, radius and 900ms/200ms timing); that remains unimplemented and was not
this phase's to change.

**`share_menu` is a `»` double-chevron in the approved package** — an
overflow-style glyph, not a share mark — and it sits beside Fast-forward's `▶▶`.
Shipped as delivered, because artwork follows behaviour and the package is the
approved source, but the two are similar in silhouette and that is an owner
observation rather than a defect.

### Verifying a greyed menu item from a screenshot does not work

The first attempt to confirm the gate measured pixels: peak label luminance read
**230 for all three rows**, and menu-icon luminance read 227/202/247 for
copy-path / copy-lucidlink / show-in-explorer — which cannot separate a disabled
row from a shorter label with a different glyph. It read a **correct build as a
broken one**, which is the failure mode phase 5's ladder leg recorded as the worse
kind.

So the gate went into the HUD instead, on the storage line beside the
classification it is built from:

| media | HUD reads |
|---|---|
| local NTFS file | `src local (fixed local volume)` · **`share path ok explorer ok lucid unavailable`** |
| same file, `TRACE_REMOTE_IO=1` | `src REMOTE (forced by TRACE_REMOTE_IO) [override]` · **`share path ok explorer ok lucid disabled`** |

**That second row is the negative control, and it is the point.** Both branches
of the gate are live and produce different answers, and **neither says `ok`** —
because the third condition holds it. A gate with one reachable branch would have
looked identical in every screenshot taken of the first row.

### What was measured, and one thing that could not be

| case | result |
|---|---|
| Copy File Path, from the overlay button | clipboard reads the full native path; status bar `File path copied.` |
| Show in File Explorer | Explorer opened on the containing folder with `M&M_TopGun_1080` **selected** (read back through `Shell.Application`, not from a screenshot) |
| local file | `lucid unavailable` |
| forced virtual mount | `lucid disabled` |
| **file removed while open** | Copy File Path stays enabled, **Show in File Explorer greys** |
| no media at all | all three rows greyed, docked Share button disabled |
| keyboard-only | `Alt`,`F`,`S` reaches the submenu; status bar carries each item's tip |

**A real `V:\` LucidLink path was NOT tested**, because `V:\` is live client
production storage and no file was nominated. The virtual-mount branch was
exercised through `TRACE_REMOTE_IO` instead. Phase 9 needs a nominated file.

**"File removed while open" took two attempts and the first one was wrong.**
Windows refuses to delete a video file Trace has open, so the obvious test cannot
run at all. A **directory junction** was tried next — open through the junction,
delete the junction — and Qt still resolved the path afterwards, so the HUD read
`explorer ok` and it looked like a gate bug. It is not: a **still image** is the
case where Trace does not hold the handle, and deleting one while it is displayed
greys Show in File Explorer exactly as intended. **The junction was not a valid
way to make a path vanish**, and the reading it produced accused the code.

### Regression

Control binary built from `45a083a` in a separate worktree and **verified by hash on every
swap** (`27BF68B4…` control, `D868D72E…` phase 8). Same **1920x1080 @ 59.999Hz display** as
phases 5–7, not the physical panel — `stalls` and `hitch` coincide at this refresh, and **no
subjective judgement was taken and none is valid from this display**. `d3d11` default.
Cadence in **overlay mode**, the shipping configuration; the drag, reverse and matrix runs in
bar mode with `TRACE_TRANSPORT_BAR=1`, `win 1280x843`, `display 640x360 filtered x3`.

| run | control | phase 8 |
|---|---|---|
| 4K H.264 cadence ×3 | 100.0 / 100.0 / 100.0%, 120 frames, `handler>budget 0 of 119` (max 4.3 / 4.7 / 5.0), p50 41.6 / 41.6 / 41.5, max 43.5 / 43.4 / 44.3 | 100.0 / 100.0 / 100.0%, 120 frames, `0 of 119` (max 4.3 / 4.1 / 4.4), p50 41.5 / 41.7 / 41.7, max 44.1 / 43.4 / 44.1 |
| 4444 cadence ×2 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 37.5 / 37.2), max 46.4 / 46.8 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 36.9 / 36.8), max 45.6 / 47.5 |
| reverse 1× ×3 | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 3.7 / 3.7 / 3.6), `hitch 0` | 100.0% all three, 114 frames / 4.75s, `0 of 113` (max 4.1 / 3.9 / 3.6), `hitch 0` |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, `walk 0f`, full-res planar, `hitch 0`, `stalls 0 of 113`, `ui gap max 67.2ms` | same landing, `hitch 0`, `stalls 0 of 113`, `ui gap max 61.7ms` |
| lifecycle | `-PlayThroughDrag` PASS 39.1%, `-PausedThroughDrag` PASS 0% | PASS 39.8%, PASS 0% |
| transitions | **25 of 25 PASS** | **25 of 25 PASS** |

Cadence buckets identical on 4K H.264 (`~1x 119`, every other bucket 0) on both binaries.
Landing exact on every run. `land` reads 0 through every shuttle press.

**The one number worth watching was `paints`, and it did not move.** The overlay emits one
more quad per frame now — the Share icon — and 4K H.264 playback reads **`paints 152/121` on
phase 8 against `151/153/152` on the control**, i.e. inside the control's own spread. **4444
is the file with the least headroom** (handler ~23ms of a 41.67ms budget) and it absorbed the
extra quad at `handler>budget 0 of 260` with 261 frames and `paints 286–289/262` against the
control's `286/262`.

**`release` moved and it is not attributable.** The `-SnapRelease` landing read **24.1ms on
the control and 3.5ms on phase 8** — an eightfold difference on a single-sample statistic,
with the landing itself identical (`target 120 shown 120 delta 0`, `walk 0f`, same
full-resolution planar destination) and `ui gap max` within 8%. Nothing in this phase touches
the scrub path. Recorded rather than claimed, in the same spirit as phase 2's 12× `p2p max`
noise and §31.2's `ui gap` asymmetry: **do not quote it as a phase 8 win.**

### What phase 8 changes about the plan

**Phase 9 is one function body plus the clipboard rules.**
`trace::app::lucidLinkIntegrationAvailable(QString& reason)` is the whole seam — the gate, the
three states, the menu, the three surfaces and the artwork all exist and are measured. It is a
named function rather than a `false` at the call site precisely so the gate reads as three
conditions now and still reads as three when one starts returning true. **Do not loosen the
classifier's role when you get there**: it stays a necessary condition, and the installed
integration is what makes the verdict Available.

**Phase 9 also needs Anj to nominate a `V:\` file before it starts**, because phase 8 could
not test a real LucidLink path at all and neither can phase 9 without one.

**`TransportBar::loadIcon` is public now**, because the Share menu's items need icons and a
second loader would be a second answer to "is the 3x file being used" — the question the `-72`
branch inside it exists to keep answerable by reading.

**A HUD field can be the right instrument for a UI state, not just for a number.** The
`share path/explorer/lucid` field exists because the obvious verification — look at the
screenshot, is the row grey — measurably does not work, and produced a confident wrong
reading. It sits on the storage line beside the classification it is built from, so the
necessary condition and the verdict it feeds are read together.

**The rate chip's real home is still unimplemented.** The approved package puts it **centred
above the transport** with its own chip styling and 900ms/200ms timing (§6); Trace draws it
inside the panel, and phase 8 moved it from top-right to top-left only to get it out of the
Share button's way. Whoever implements §6 properly should know the current position is an
approximation twice over.

**Still no non-file media source exists**, so "disable for non-file sources" is enforced by
construction (`fileBacked` is asked of the `MediaItem`, not inferred from the path) and has
never executed with a real non-file source. The no-media case is the closest thing that runs.

---

## Phase 9 — the LucidLink shell-integration prototype (2026-08-11)

### The mechanism, and why the obvious one is the wrong one

The requirement's order is: a supported API first, then the registered shell
command. **Both were investigated and the first one loses on its own merits.**

**LucidLink runs a local REST service and it is authoritative.** The CLI's
`--rest-endpoint` option is its front door; the daemon serving `V:\` is instance
501 on port 8279. For the nominated file:

```
GET /fsEntry?path=/Live/.../Universe_Full_Takes_v005_16x9.mp4
  { "id" : "2955:105901", "type" : "file", "size" : 12496458, ... }
```

`2955:105901` is **exactly** the identifier in the expected link. But no endpoint
returns a link — it returns the *ingredients*, and assembling
`lucid://<filespace>/file/<id>/<name>?reveal=true` from them is hard-coding
LucidLink's URL format, which the requirement forbids outright. The vendor's own
extension does that assembly internally: `LucidShellExt.dll` carries the literals
`lucid://`, `/file/`, `?reveal=true`, `/fsEntry?path=` and `127.0.0.1`. **That
format belongs to LucidLink**, newer installations may emit an
`app.lucidlink.com` HTTPS link instead, and the only way to accept "whatever the
installed integration actually produces" is to make the integration produce it.

So the shipped mechanism is the **registered shell command, invoked through shell
interfaces** — the requirement's option 2, and explicitly not screen automation.
The REST API is not wasted: it is what proves the id in a returned link is the
one the daemon holds, and it is the right tool if that check is ever wanted.

### Only the Lucid handler is created, and that is a safety property

The merged Explorer context menu for the nominated file carries items from a
dozen vendors — Adobe, OneDrive, PowerToys, Tailscale, Copilot. Building it loads
every one of those DLLs into the calling process, which is not something a media
player should do to itself, and it puts "Copy link" in a menu whose composition
Trace does not control.

`CoCreateInstance` on the one CLSID, `IShellExtInit::Initialize` with the file's
`IDataObject`, then `QueryContextMenu` on a private popup, yields a menu with
**only LucidLink's commands in it**:

```
LucidContextMenu Initialize -> 0x00000000
QueryContextMenu -> 0x00000005 (5 items)
  item id=2 text='Pin'
  item id=3 text='Copy link'
SUPPORTED via LucidContextMenu (offset 2)
```

**The item next to the one Trace wants is `Pin`, and pinning hydrates the file
onto the mount.** The mount is live client production storage. That is why
identification is an exact, case-insensitive match on the display text and never
positional, and why a miss reports unavailable rather than falling back to
anything.

The CLSIDs are **discovered from the registry, not hard-coded**: the
`ContextMenuHandlers` keys are enumerated and those whose registration name says
Lucid are taken, with the `InprocServer32` path used to corroborate. Both
generations are present on this box and both are tried —
`LucidContextMenu {b5fd958e-...}` under `Program Files\Lucid`, and
`LucidLinkContextMenu {2BAF8C7E-...}` under `Common Files\LucidLink`.

### The display-string risk, stated because it is real

**The extension exposes no canonical verb.** `GetCommandString(GCS_VERBW)` fails
for every item it contributes, so there is nothing stable to ask for by name and
the item must be recognised by the text it renders. The requirement anticipates
exactly this and asks for it to be documented before shipping:

- measured against **LucidShellExt 1.0.15**, which renders `Copy link`;
- a **localized** Windows would render a translated string, and Trace would then
  report the integration as unavailable rather than invoke the wrong item;
- **failing closed is the design**, for the `Pin` reason above.

### The gate: what makes it Available is the integration's own answer

The storage classifier stays a **necessary** condition and can still only move
the verdict from Unavailable to Disabled. What supplies Available is
`probeLucidSupport()` — and the discrimination is the extension's own: on a file
outside a linked filespace `IShellExtInit::Initialize` returns **E_INVALIDARG**
and no command is offered.

| case | HUD gate | evidence |
|---|---|---|
| local NTFS file | **`lucid unavailable`** | the `[lucid]` log is **empty** — the probe is never started, so no COM and no third-party DLL is loaded for a local file at all |
| local file, `TRACE_REMOTE_IO=1` (eligible, integration declines) | **`lucid disabled`** | both handlers `Initialize -> 0x80070057`, then `no handler offered a copy-link command` |
| **nominated LucidLink file** | **`lucid ok`** | `SUPPORTED via LucidContextMenu (offset 2)` |

The middle row is the negative control the requirement names directly —
*eligible LucidLink files without a working integration: disabled, not falsely
available* — and it is a real path through the code rather than a simulated one,
because `TRACE_REMOTE_IO` forces only the classifier and leaves the extension to
answer honestly.

### The link

Driven from the **overlay's** Share menu, on the nominated file:

```
InvokeCommand(offset 2) -> 0x00000000
clipboard accepted after 21ms:
lucid://projects.omcprod/file/2955:105901/Universe_Full_Takes_v005_16x9.mp4?reveal=true
```

Compared with `8_LucidLink\LucidLink.txt` using a **case-sensitive** comparison:
**exact match**. The clipboard held a sentinel immediately before the press.

The clipboard is snapshotted before invocation, the change is waited for by
`GetClipboardSequenceNumber` with a 4s timeout, and the result is validated as a
supported link form (`lucid://` or `https://app.lucidlink.com/`, one token, no
whitespace) — **anything else is rejected and the previous value is put back**. A
timeout leaves the clipboard untouched and says so. Trace never writes a composed
value. One limitation, stated rather than hidden: only `CF_UNICODETEXT` is
snapshotted, so a clipboard holding an image or a file list cannot be restored.

### The instrument was the bug, and it nearly became a mechanism

The first build read **`lucid disabled`** on the nominated file. Switching the
worker's apartment from `CoInitializeEx` to `OleInitialize` made it read
`lucid ok`, and "a shell extension needs the full OLE stack" is an appealing
explanation that was about to be written down as the fix.

**It is wrong.** That build also failed to call `refreshHud()` after the probe
landed, and a paused file does not refresh on its own — so the HUD was showing
the state from media-open time and said nothing about the probe at all. The
**menu**, which reads live state, had been correct the whole time.

`TRACE_LUCID_COINIT=1` is the control that settles it, retained the way
`TRACE_LONGGOP_SLICE_THREADS` is. Measured on the nominated file:

| apartment | Initialize | verdict |
|---|---|---|
| `OleInitialize` (default) | `0x00000000` | `SUPPORTED (offset 2)` |
| `CoInitializeEx` (`TRACE_LUCID_COINIT=1`) | `0x00000000` | `SUPPORTED (offset 2)` |

`OleInitialize` is kept as a **precaution** — it is what Explorer does — and is
not claimed to have fixed anything. **A stale instrument accuses the code, and
this is the second time in two phases**: phase 8 read menu-icon luminance and
called a correct build broken.

`TRACE_LUCID_LOG=1` exists because of this. The gate is three refusals deep and
`disabled` looks identical whether the handler was not found, the extension
declined the file, or the menu had no matching item.

### The 1x1 and 4x5 ProRes assets

Both are 23.976 ProRes 10-bit, 528 frames, carrying a **non-drop start timecode
of `00:59:53:00`**, read from the container and not synthesised:

| | encoded | video rect | frame 0 | frame 24 |
|---|---|---|---|---|
| 1x1 | 1080x1080 | square, pillarboxed | `00:59:53:00` | `00:59:54:00` |
| 4x5 | 1080x1350 | `display 288x360` | `00:59:53:00` | `00:59:54:00` |

Twenty-four frames at 23.976 non-drop is exactly one timecode second, and both
files step it correctly. `TRACE_OPEN_LOG` reports `timecode=00:59:53:00` for
each, so the extraction and the readout agree rather than being two answers.

**CPU and D3D11 framing agree exactly** on the 4x5: `display 288x360` and
`win 1280x767` on both backends. (`filtered x2` against `x1` is the step-9
reduction tap count, a GPU-path figure, not a framing one.)

**One defect is carried forward rather than fixed, per instruction.** The
floating transport is **460 logical px wide against a 288px video rect** on the
4x5 at the default window size, so the panel is 1.6x wider than the picture: it
overhangs the image on both sides and covers the lower part of it. The transport
remains usable — it reveals, hides and responds normally — but it occupies far
more of a 1x1 or 4x5 picture than of a 16:9 one. This is an **owner
visual-review item**, not a phase 9 blocker, and fixing it is aspect-ratio and
window-sizing work the instruction explicitly excludes. Note the approved
package's section 8 "media-shaped window" would change the premise entirely,
since the window would adopt the source display aspect ratio and the panel would
be sized against a picture that fills it.

### Regression

Control binary built from `579ffaa` in a separate worktree and **verified by hash on every
swap** (`07179488…` control, `43784BDE…` phase 9). Same **1920x1080 @ 59.999Hz display** as
phases 5–8. `d3d11` default. Cadence in overlay mode; the drag, reverse and matrix runs in bar
mode with `TRACE_TRANSPORT_BAR=1`, `win 1280x843`, `display 640x360 filtered x3`.

| run | control | phase 9 |
|---|---|---|
| 4K H.264 cadence ×2 valid | 100.0 / 100.0%, 120 frames, `handler>budget 0 of 119` (max 4.4 / 4.4), p50 41.6 / 41.7, max 43.5 / 43.5 | 100.0 / 100.0%, 120 frames, `0 of 119` (max 4.2 / 4.5), p50 41.7 / 41.7, max 43.3 / 44.0 |
| 4444 cadence ×1 valid | 99.8%, 261 frames, `0 of 260` (max 38.3), max 46.6 | 99.8%, 261 frames, `0 of 260` (max 36.7), max 45.6 |
| reverse 1× ×3 | 88.2 / 100.0 / 100.0% | 88.1 / 88.2 / 100.0% |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, `walk 0f`, full-res planar, `hitch 0`, `stalls 0 of 113`, `ui gap max 67.0ms`, `release 23.6ms` | same landing, `hitch 0`, `stalls 0 of 113`, `ui gap max 69.5ms`, `release 24.2ms` |
| lifecycle `-PlayThroughDrag` ×3 | **PASS 39.6 / 39.6 / 39.6%** | **PASS 39.6 / 40.3 / 39.6%** |
| lifecycle `-PausedThroughDrag` | PASS 0% | PASS 0% |
| transitions | **25 of 25 PASS** | **25 of 25 PASS** |

Cadence buckets identical on 4K H.264 (`~1x 119`, every other bucket 0) on both binaries.
`paints 152/121` on both, so the phase adds nothing to the paint path — it adds no drawing at
all. Landing exact on every run.

**Reverse 1× went bimodal on both binaries and that is the known property of the gesture, not
a regression.** The two populations are exactly the recorded ones — `frames 114 / elapsed
4.75s` at 100.0%, or `frames 97 / 4.58s` at 88.1–88.2% — and **both binaries produced both**.
The split differs (control 1 of 3 slow, phase 9 2 of 3) and three runs cannot distinguish
those. This is the reason that gesture has always been recorded as "take three", and it is
also why the `SNAP gop 2` item was closed rather than carried: a single run of it supports
nothing.

**The first cadence rep of each file is VOID on both legs, in the same way.** It reads
`presented -- / 24.00 fps | frames 0`, `paints 0/1`, `cadence no samples yet` — the capture
lands before playback has produced anything, on the first run after a binary swap. It is
symmetric across the two legs so it does not bias the comparison, and phase 5 recorded a
related first-run-after-swap outlier. **Discard rep 1 or give cadence an extra repeat.**

### The lifecycle FAIL was the harness, and it is the sharpest instance yet of a leg that could not pass

Both legs first reported `-PlayThroughDrag: FAIL - picture frozen (0%)`. Re-run three times
per binary: **3 of 3 FAIL on phase 9 and 3 of 3 FAIL on the control** — the same phase 8 binary
that passed this gesture at phase 8. That symmetry said it was not phase 9's, and the cause
turned out to be in `lifecycle.ps1`:

**`SetForegroundWindow` does not work from a background process.** Windows refuses foreground
activation to a process that does not own the current foreground window; the call returns, and
`GetForegroundWindow()` still names something else. Measured directly:
`SetForegroundWindow worked: False (fg=6688278 trace=22416776)`, and a Space keystroke after
it moved the picture by **0**. The same sequence preceded by a synthetic **click** on the title
bar reads `foreground is Trace now: True` and a picture delta of **3663**.

**Why it hid, and why it is worse than a check that cannot fail**: the mouse-driven gestures
are unaffected, because `mouse_event` goes wherever the cursor is. Only `-PlayThroughDrag`
needs a keystroke — it starts playback with Space — so a lost keystroke leaves the app paused
and the check reports exactly the product regression it exists to catch. And **its control
could not fail**: `-PausedThroughDrag` expects no motion, so it passes whether or not its
keystrokes arrive. The pair read as "the feature broke and its control is fine" when the truth
was "one leg never ran".

`lifecycle.ps1` now takes focus with a real click when `GetForegroundWindow()` disagrees, and
**exits with `FOCUS FAIL` if it still cannot** — so the failure can never again be mistaken for
a frozen picture. With that fix: **3 of 3 PASS on both binaries**, 39.6–40.3%.

The title bar is clicked rather than the video, because a click on the video reveals the
overlay and a second one inside the double-click interval toggles fullscreen.

### What phase 9 changes about the plan

**Phase 10 is the open phase and is wiring only** — five `QAction`s onto
`viewer_->setViewTransform()` plus a reset on new media. `TRACE_VIEW_TRANSFORM`
leaves with it, the way `TRACE_SHUTTLE_ENTRY` left with phase 5. The 1x1 and 4x5
assets are the right material: a square clip makes a quarter turn's letterboxing
arithmetic visible in a way 16:9 does not.

**`lifecycle.ps1` no longer trusts `SetForegroundWindow`, and every other script
that sends keystrokes should be read with that in mind.** `transitions.ps1`,
`revplay.ps1` and `overlay_ladder.ps1` all drive keys at some point. They passed
throughout this session, so whatever they do works today — but the failure mode
is silent, one-sided and indistinguishable from a product regression, so if any
of them ever reports a frozen picture, **check `GetForegroundWindow()` before
believing it**.

**Cadence's first repeat after a binary swap is unreliable.** Both legs of this
phase produced a void rep 1 (`frames 0`, `paints 0/1`). Ask `cadence.ps1` for one
more repeat than needed, or discard the first.

**The Share menu now has a command that can take seconds.** Copy LucidLink Link
reaches a daemon, so it runs on a worker with a 4s clipboard timeout and a
one-at-a-time guard. Anything else that talks to LucidLink should inherit that
shape rather than block a click.

**`hasSourceTimecode_` held up on new material.** The 1x1 and 4x5 assets are the
first production files in the set with a non-zero, non-drop start timecode other
than the 4444 clip, and the extraction, the readout and `TRACE_OPEN_LOG` all
agree on `00:59:53:00`. Phase 7's single-gate design needed no change.

**Two owner visual-review items are carried, neither blocking**: the Share glyph
is a `>>` double-chevron beside Fast-forward's `>>`-with-triangles, and **the
floating transport is wider than the picture on 1x1 and 4x5 media** (460 logical
px against a 288px video rect on the 4x5). Both are recorded in
`docs/next-session-prompt.md`.

---

## Phase 10 — temporary view transforms (2026-08-11)

### What shipped

Five shared `QAction`s — Rotate Left, Rotate Right, Flip Horizontal, Flip
Vertical, Reset View Transform — in a real **Edit** menu, which is where the spec
puts them. A top-level menu rather than another File submenu: unlike phase 7's
Time Display group, this one *is* a whole menu in the spec, and the only other
thing the spec puts in Edit is Copy Current Frame, which is later and
conditional.

**Wiring only, as briefed.** The renderer-neutral contract was built and measured
at plan §31 (`4b7174f`); neither backend needed a line, and `TRACE_VIEW_TRANSFORM`
left with the phase that made it redundant — the way `TRACE_SHUTTLE_ENTRY` left
with phase 5.

`applyViewTransform()` is the one place the transform changes, so what is on
screen and what the menu says come from a single write followed by a single
read-back. It asks the *viewer* for the resulting transform rather than assuming
the requested one took.

### Rotation rotates what the user sees, and that is the determinism requirement

The composition is `screen = flip(rotate(source))`, so the **flip** buttons
already act on what is visible. Rotation does not come for free, and this is the
whole of the combined-rotate-and-flip question:

**A mirror reverses the sense of a rotation applied after it** — for a mirror
`M`, `R(t) . M == M . R(-t)`. So with exactly one mirror in force, Rotate Right
must **decrement** `quarterTurns` or the picture visibly turns **left**. With both
mirrors it must not, because H then V is a 180° rotation and rotations commute
with each other.

`ViewTransform::rotatedOnScreen()` owns that, so both backends and every caller
inherit one answer. Flips need no compensation and are plain toggles: being
screen-space already makes each its own inverse.

**Verified by an independent landmark rather than by trusting the arithmetic.**
The 4×5 slate has a black bar in one corner:

| state | black bar | HUD |
|---|---|---|
| identity | bottom-right | `display 288x360` |
| Flip Horizontal | bottom-left | `display 288x360 view flipH` |
| then Rotate Right | **top-left** | `display 450x360 view rot270 flipH` |

Top-left is where a **clockwise** turn puts a bottom-left mark. The naive
implementation would have recorded `rot90 flipH` and turned the picture the other
way.

### The fit and the reduction taps come from the post-transform fit

Measured across the full rotation cycle on 4K H.264, which is the case plan §31
predicted numbers for:

| presses | HUD |
|---|---|
| identity | `display 640x360 filtered x3` |
| ×1 | `display 202x360 filtered x4 view rot90` |
| ×2 | `display 640x360 filtered x3 view rot180` |
| ×3 | `display 202x360 filtered x4 view rot270` |
| ×4 | `display 640x360 filtered x3` — identity, `view` gone |

`202x360` and `x3 → x4` are §31's predicted values to the digit. A quarter turn
re-letterboxes and the taps follow the post-transform fit; 180° changes neither,
which is the check that the taps track the *fit* and not the rotation.

On the 4×5 the fit goes `288x360 → 450x360` at 90°, and on the 1×1 it stays
`360x360` — a square is invariant under rotation, which is the degenerate case
worth having in the set.

### repaint(), not update() — the HUD was reporting the previous transform

The fit and the taps are measured **by** the paint and reported afterwards, so
refreshing the HUD after a merely-scheduled repaint prints the previous
transform's `display`. On a paused file nothing refreshes it again, so it stays
wrong.

**Measured before the fix**: the 4×5 rotated 90° drew visibly landscape while
`display` still read `288x360`. The picture was right and the instrument was
wrong. Same reason the scrub walk calls `repaint()`.

**Third stale-instrument finding in three phases** — phase 8 read menu-icon
luminance, phase 9 read a HUD that had not been refreshed after the LucidLink
probe, and here the HUD had not been refreshed after the paint. In all three the
code was correct and the instrument accused it.

### CPU and D3D11 agree on orientation, fit and framing

The plan warned these could differ by a mirror, because QPainter post-multiplies
and the CPU path names `scale` before `rotate` deliberately. They do not.

`display` and `win` are identical on both backends at every state:

| state | d3d11 | cpu |
|---|---|---|
| rot90 | `display 450x360 view rot90`, `win 1280x843` | identical |
| rot90 + flipH | `display 450x360 view rot90 flipH` | identical |
| flipV | `display 288x360 view flipV` | identical |

Pixel difference over the video band, docked bar so the overlay's fade state is
not in the comparison (`scripts/measure/banddiff.ps1`):

| state | differing px | max channel delta |
|---|---|---|
| identity | 3524 of 446600 (0.79%) | 141 |
| rot90 | 4661 (1.04%) | 154 |
| rot90 + flipH | **4661 (1.04%)** | **154** |
| flipV | 3475 (0.78%) | 141 |

**rot90 and rot90+flipH agreeing to the pixel is evidence, not coincidence.**
Flipping both captures maps the difference map onto its mirror, which preserves
the count and the maximum exactly — so an identical pair is what an *exact*
mirror on both backends predicts. A mirror disagreement would instead have shown
the picture in two places and a difference near 50%.

The residual is edge resampling on a high-contrast slate, and it is *lower* at
identity than at rot90 because the rotated fit covers more area. **The first
attempt at this measurement read 9.1%** and was the floating overlay's fade
state landing inside the band — the panel is composited over the video, so a
cross-backend diff has to be taken in bar mode.

### The transform is viewing state and survives the transport

Applied `Rotate Right`, then drove everything, capturing after each. `view rot90`
is present in all nine states, and the frame index advances normally underneath
it:

`01-rot` · `02-playing` · `03-paused` · `04-stepped` · `05-shuttle` ·
`06-stopped` · `07-scrubbed` · `08-fullscreen` · `09-windowed`

`display 450x360 view rot90 | win 1280x843` reads identically in every windowed
state; fullscreen reads `display 1196x957 filtered x1 view rot90 | win 5120x1440`
and windowing back returns to `450x360`. No decoder request is made — the frame
on screen is the frame that was already there, drawn through a different
coordinate transform.

**Frame numbering and source timecode are untouched.** Frame index reads 0 → 60 →
157 across those states, matching `Frame:` exactly. With `rot90` applied to the
1×1, `Timecode:` reads `00:59:53:00` at frame 0 and `00:59:54:00` at frame 24 —
the same values the untransformed file gives. The share gate is unchanged.

**Reset works both ways.** `Rese&t View Transform` returns the 4×5 to
`display 288x360` with the `view` field gone; opening a different file through
File ▸ Open with `rot90 + flipH` in force opens it upright with no `view` field.

Two notes on the menu. **Reset has no shortcut on purpose**: the approved package
puts it on `Ctrl+0` and the interface spec gives `Ctrl+0` to Actual Size. The
spec governs, its rule on conflict is to preserve the existing binding, and
Actual Size does not exist yet — so this phase claims neither and leaves the key
for whoever adds it. `Ctrl+L` / `Ctrl+R` are unclaimed in both documents and are
taken. And it is **"Rese&t"**, not "&Reset", because Rotate Right already owns R
in that menu and two items sharing a mnemonic makes the key cycle the highlight
instead of activating either.

### Regression

**THIS SESSION'S DISPLAY CHANGED PART-WAY THROUGH, and the regression is on the new one.**
Parsec disconnected and the **physical panel took over — 5120x1440 @ 239.999Hz** — where
phases 5–9 all ran on Parsec's 1920x1080 @ 59.999Hz. Both legs below were run on the panel, so
the A/B is valid; but `stalls` is `2 × refresh` and its bar is **8.3ms here against 33.3ms in
phases 5–9**, so **no stall figure below is comparable with those tables**. `hitch` is a fixed
33ms bar and still is. The visual and geometry results earlier in this section are windowed
measurements of the client area and are unaffected.

Control binary built from `485cdab` in a separate worktree and **verified by hash on every
swap** (`07179488…` → rebuilt here, `536FFA67…` phase 10). `d3d11` default. Cadence in overlay
mode; drag, reverse and matrix runs in bar mode with `TRACE_TRANSPORT_BAR=1`.

| run | control | phase 10 |
|---|---|---|
| 4K H.264 cadence ×3 | 100.0 / 100.0 / 99.9%, 120 frames, `handler>budget 0 of 119` (max 4.4 / 4.5 / 4.6), p50 41.8 / 41.7 / 41.8, max 44.2 / 43.2 / 43.7 | 100.0 / 100.0 / 99.9%, 120 frames, `0 of 119` (max 4.3 / 4.3 / 4.5), p50 41.8 / 41.9 / 41.9, max 44.1 / 43.5 / 44.1 |
| 4444 cadence ×2 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 40.0 / 38.9), max 45.0 / 44.2 | 99.8 / 99.8%, 261 frames, `0 of 260` (max 38.4 / 37.1), max 44.9 / 48.2 |
| reverse 1× ×3 | 100.0 / 84.6 / 88.2% | 100.0 / 88.1 / 100.0% |
| `scrub -SnapRelease` | `target 120 shown 120 delta 0`, `walk 0f`, full-res planar, `stalls 104 of 114 (>8.3ms)`, `hitch 0`, `ui gap max 65.7ms`, `release 24.8ms` | same landing, `stalls 103 of 114`, `hitch 0`, `ui gap max 66.4ms`, `release 22.4ms` |
| lifecycle | `-PlayThroughDrag` PASS 40.1%, `-PausedThroughDrag` PASS 0% | PASS 39.1%, PASS 0% |
| transitions | **25 of 25 PASS** | **25 of 25 PASS** |

Cadence buckets identical on 4K H.264 (`~1x 119`, every other bucket 0) on both binaries.
Landing exact on every run. **`paints` is unchanged** — 151–152 of 121 on both — which is what
says the phase adds no drawing: at identity the transform is not applied at all, and when it is
applied it changes a coordinate rather than adding a pass.

**Reverse 1× is bimodal on both binaries again**, into the recorded populations —
`114 frames / 4.75s` at 100.0% or `97 / 4.58s` at 88.1–88.2%, plus one control run at
`99 / 4.88s` and 84.6%. Control was 2 of 3 slow and phase 10 1 of 3; three runs cannot
distinguish those, which is why that gesture has always been recorded as "take three". Phase 10
reading *better* on this sample means nothing.

**`lifecycle.ps1`'s phase 9 focus fix held.** Both `-PlayThroughDrag` legs passed first time on
both binaries, where before the fix the same gesture reported 3 of 3 FAIL on two binaries that
were both fine.

**No void first repeat this time.** Phase 9's legs each produced a `frames 0` rep 1 after a
binary swap; on the panel all five cadence runs per leg are valid. The advice stands — give
cadence an extra repeat — but the cause looks like machine speed rather than anything
structural.


### What phase 10 changes about the plan

**Phase 11 is Open Recent**, and it is mostly a set of refusals — no startup
probing, no blocking on disconnected LucidLink paths, canonical stored paths, a
missing entry reported rather than silently dropped. `MediaShare`'s
`canonicalNativePath` already exists; do not write a second normalisation.

**`ViewTransform::rotatedOnScreen()` is the one place the rotate-versus-mirror
rule lives.** Any new way to rotate calls it rather than touching
`quarterTurns`. **`applyViewTransform()` is the one place the transform changes**,
and it calls `repaint()` before `refreshHud()` on purpose — a caller that uses
`update()` will silently report the previous transform's `display`.

**`scripts/measure/banddiff.ps1` and `scripts/measure/viewtransform.ps1` are new.**
The first is a LockBits pixel diff over a band of rows, which runs in seconds
where a `GetPixel` loop over the same band took minutes and would simply have
stopped being run. The second drives the five actions and captures. **Take
cross-backend diffs in bar mode** — the floating overlay is composited over the
video and its fade state read 9.1% of the band on the first attempt here.

**`Ctrl+0` is unclaimed and spoken for by two documents.** The approved package
wants it for Reset View Transform, the interface spec for Actual Size. Whoever
adds Actual Size should take it; Reset stays shortcut-less unless the owner rules
otherwise. Worth raising with the owner at phase 13, when the Keyboard Shortcuts
window makes every binding visible at once.

**The `display` field is one paint stale immediately after opening media**, and
that is pre-existing rather than phase 10's: `openPath` ends with `refreshHud`
before the first paint of the new file. It became visible here only because the
previous file's transform-affected fit is what lingers. Not fixed, because
changing the open path's paint behaviour would move the `open ...ms` figures the
regression quotes.

---

## Session closeout — 2026-08-11 (phases 8, 9 and 10)

**An index, not a second copy.** Every fact below is recorded in full in a phase
section above or in `docs/next-session-prompt.md`. If this ever disagrees with
the section it points at, the section wins.

**This section moved.** It used to sit between phase 7 and phase 8, where it read
as if the document ended there. A closeout belongs at the end.

### Phase status

| phase | state | commit |
|---|---|---|
| 1 audit | done | `7abb6a5` (`docs/interface-pass-1-audit.md`) |
| 2 shared actions and artwork | done | `58bfca6` |
| 3 stepping and shuttle contracts | done | `4de678e` |
| 4 forward shuttle | done | `e559d07` |
| 5 reverse shuttle | done | `90140f9` |
| 6 fullscreen consolidation + overlay auto-hide | done, **owner sign-off** | `bc84431` (CI 90) |
| 7 Time Display + zero-based frame UI | done | `f15e368` (CI 92) |
| **8 Share menu + ordinary path copying** | **COMPLETE** | `a6447aa` + `f39eb67` (CI 94) |
| **9 LucidLink shell-integration prototype** | **COMPLETE, owner accepted** | `9b62ab0` (CI 96) |
| **10 Temporary view transforms** | **COMPLETE** | `d2b4481` (CI 98) |
| **11 Open Recent** | **NEXT** | — |

Phases 4–5 complete the transport redesign; 6 makes the floating overlay the only
transport; 7 makes the time readout honest; 8 adds the Share menu and the
LucidLink *gate*; 9 makes the LucidLink link real; 10 wires the view transforms.
**Phase 11 is the next starting point.** Full brief in
`docs/next-session-prompt.md`.

### Owner decisions taken this session, all settled

1. **Accessibility** — the alpha ships with the composited overlay invisible to a
   screen reader, and **phase 13 BUILDS an accessibility proxy tree rather than
   polishing one**. Estimate it as construction. Do not re-raise it as a
   question.
2. **`SNAP gop 2`** — no longer tracked. One run of six on a binary predating
   phase 4; re-open only on a real complaint, and take three runs **at the panel**.
3. **Phase 9 accepted**, with the LucidLink display-string dependency and the
   fail-closed behaviour to be preserved. Not to be redesigned during later
   phases.
4. **The Share glyph is not to change** without a further decision.

### Two owner visual-review items are carried, neither blocking

- **The Share glyph is a `>>` double-chevron** (the approved package's
  `share_menu`) beside Fast-forward's filled `>>`. Similar in silhouette.
- **The floating transport is wider than the picture on 1x1 and 4x5 media** — 460
  logical px against a 288px video rect on the 4x5, so it overhangs the image and
  covers its lower part. **Carried to the media-shaped window work by owner
  instruction**; the approved package's section 8 would change the premise rather
  than needing a panel fix.

### One thing the owner may want to rule on at phase 13

**`Ctrl+0` is claimed by two documents.** The approved package assigns it to
Reset View Transform; the interface spec assigns it to Actual Size. Phase 10
claimed neither and left Reset shortcut-less, because the spec governs and its
conflict rule is to preserve the existing binding. Phase 13 renders the Keyboard
Shortcuts window, which is where every binding becomes visible at once.

### What this session established that outlives it

1. **A gate's authority has to come from the thing being gated.** The storage
   classifier is a *necessary* condition for LucidLink and can only ever move the
   verdict from Unavailable to Disabled; what makes it Available is the installed
   integration's own answer for the specific file. Phase 8 and 9 sections.
2. **The vendor owns its URL format.** LucidLink's REST API is authoritative for
   identifiers and returns no link, so Trace asks the extension to produce one
   rather than assembling `lucid://.../file/...?reveal=true` itself. Phase 9.
3. **Rotation must rotate what the user SEES.** A mirror reverses the sense of a
   rotation applied after it, so with one flip in force Rotate Right must
   decrement the quarter turns. `ViewTransform::rotatedOnScreen()`. Phase 10.
4. **THREE STALE-INSTRUMENT FINDINGS IN THREE PHASES, and in all three the code
   was right and the instrument accused it.** Phase 8 tried to read a disabled
   menu item's greyness from icon luminance; phase 9 read a HUD that had not been
   refreshed after the LucidLink probe and nearly recorded `OleInitialize` as a
   fix that a control then refuted; phase 10 read a HUD refreshed before the
   paint that measures the fit. **Before believing a reading, ask when it was
   taken.**
5. **A harness leg that cannot pass is worse than one that cannot fail, and its
   control can hide it.** `lifecycle.ps1` relied on `SetForegroundWindow`, which
   Windows refuses to a background process; the keystroke that starts playback
   went elsewhere and `-PlayThroughDrag` reported a frozen picture on two
   binaries that were both fine. Its control, `-PausedThroughDrag`, expects no
   motion and passed regardless. Fixed, and it now exits `FOCUS FAIL` rather than
   accusing the app.

### Settled earlier in the pass, indexed here so this stays a whole-pass closeout

1. **Fast-forward begins at 2x and advances +2x -> +5x -> +10x -> +30x**, capping
   at 30x. Phase 4 section; ladder re-confirmed from the button at phase 5.
2. **The Right arrow is the only next-frame surface.** `nextFrameAction_` itself
   is untouched -- the spec removes the button, not the command. Phase 4.
3. **Previous-frame remained visible through phase 4 and left at phase 5**, which
   is the artwork-follows-behaviour rule working rather than an oversight: for
   exactly one commit `OverlayHooks` read `stepBack` beside `fastForward`.
   Phases 4 and 5.
4. **`landPreviousExactly` was removed after measurement showed it bought no
   anchoring** -- the landing is a reverse-cache hit by construction, and a cache
   hit sets `currentFrame_` but never `lastDecodedFrame`. Stops still land.
   Phase 4.
5. **The transition and overlay harnesses exercise real interactions now.**
   `transitions.ps1` replaced `revtransitions.ps1` on a run-boundary axis and was
   re-derived again at phase 5 (25 cases); `overlay.ps1` had been aiming 1.2px
   outside every control. Phases 4 and 5.
6. **The phase 2 overlay interaction evidence was invalid and the corrected test
   passes** -- `08-mid-drag` reads 0 px, max delta 1, and state 07 reads
   `speed -2.00x | Reverse Play` on both backends. Phases 4 and 5.

### Closeout verification

- `main` at `9116ab9`, level with `origin/main`, **0 unpushed commits**.
- **Working tree clean**; stash empty and no worktrees left, so none of the four
  control-binary A/Bs stranded anything.
- **CI runs 94, 96 and 98 green** on the phase 8, 9 and 10 commits, each with
  `Verify package is launchable` and `Verify the renderer initializes` checked
  individually rather than by the overall conclusion.
- **No prerelease published and no tag created.**
- No `Trace.exe`, build, harness or polling process left running; the Explorer
  window opened by the phase 8 Show-in-Explorer test was closed.
- **`V:\` was never written to.** Phase 9 used only the nominated file and
  invoked only the copy-link command; `Pin`, the item beside it, was never
  reached because identification is an exact text match.
- **The display changed part-way through this session** — Parsec disconnected and
  the physical panel took over. Phase 8 and 9 regressions are on **1920x1080 @
  59.999Hz**; phase 10's is on **5120x1440 @ 239.999Hz**. `stalls` is
  `2 x refresh`, so its bar moved 33.3ms to 8.3ms and **no stall figure crosses
  that boundary**. `hitch` does.
