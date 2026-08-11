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

## Session closeout — 2026-08-11

**An index, not a second copy.** Every fact below is recorded in full somewhere
above or in `docs/next-session-prompt.md`; this section exists so a reader can
confirm the phase status and the settled decisions without reconstructing them
from three documents. If it ever disagrees with the section it points at, the
section it points at wins.

### Phase status

| phase | state | commit |
|---|---|---|
| 1 audit | done | `7abb6a5` (`docs/interface-pass-1-audit.md`) |
| 2 shared actions and artwork | done | `58bfca6` |
| 3 stepping and shuttle contracts | done | `4de678e` |
| **4 forward shuttle** | **COMPLETE** | `e559d07` |
| **5 reverse shuttle** | **COMPLETE** | `90140f9` |
| **6 fullscreen consolidation + overlay auto-hide** | **NEXT** | — |

Phases 4 and 5 together complete the **transport redesign**. Phase 6 is the next
starting point and is the phase most likely to cost performance, because it
removes `transportBar_` from the layout in favour of the floating overlay and
therefore moves the video rect. Full brief in `docs/next-session-prompt.md`.

### What is settled, and where the evidence is

1. **Fast-forward begins at 2× and advances +2× → +5× → +10× → +30×**, capping at
   30×. Phase 4 section; ladder re-confirmed from the button at phase 5.
2. **The Right arrow is the only next-frame surface.** `nextFrameAction_` itself
   is untouched — the spec removes the button, not the command. Phase 4 section.
3. **Previous-frame remained visible through phase 4 and left at phase 5.**
   Stated as it happened rather than as it was planned: for exactly one commit
   `OverlayHooks` read `stepBack` beside `fastForward` and one scan glyph sat
   beside one frame-step glyph, which is the artwork-follows-behaviour rule
   working. At phase 5 the button became Rewind, `prev-frame` left the asset tree
   and the `.qrc`, and the Left arrow became the only previous-frame surface.
   Phase 4 and phase 5 sections.
4. **`landPreviousExactly` was removed after measurement showed it bought no
   anchoring.** The landing is a reverse-cache hit by construction, and a cache
   hit sets `currentFrame_` but never `lastDecodedFrame`, so it does not move the
   decoder at all — 4K H.264 `land 0.8ms`, 1080p `0.3ms`, ProRes 4444 `25.2ms`,
   with the following forward run identical in every case. Both halves of the
   recorded justification had also expired. Stops still land. Phase 4 section.
5. **The transition and overlay harnesses exercise real interactions now.**
   `transitions.ps1` replaced `revtransitions.ps1` on a run-boundary axis at
   phase 4 and was re-derived again at phase 5 (25 cases); `overlay.ps1` had been
   aiming 1.2px outside every control and is located by difference now. Phase 4
   and phase 5 sections.
6. **The phase 2 overlay interaction evidence was invalid, and the corrected live
   test passes.** With nothing registering, all twelve captures were the same
   paused frame, so the recorded `312 px (0.619%)` was the video band's own
   backend difference rather than overlay agreement. With the legs live,
   `08-mid-drag` reads **0 px, max delta 1**, and the re-pointed hooks are
   confirmed *executed* — `overlay.ps1` state 07 reads `speed -2.00x |
   Reverse Play` on `d3d11` and on `cpu`. Phase 4 and phase 5 sections.

### Carried loose end — NOT a phase 4 or phase 5 regression

**Reverse 1× is bimodal on the `revplay` gesture, and the keyframe grid can be
learned as 2 so that snapping engages at stride 1** (`SNAP gop 2`, `sched tick
81ms`, 72.5% of real time). It was observed on the **phase 4 control binary**,
i.e. it **predates phase 4**, and the control produced the worst run of the six.
It is named here as a pre-existing loose end and is explicitly **not classified
as a regression of either phase**; it was not investigated during closeout.

Two things to keep with it. A **single run of that gesture cannot support a claim
in either direction** — take three. And phase 5 saw neither the slow mode nor
`SNAP gop 2` in six runs, **which is not evidence it is fixed**: those runs were
on a 1920x1080 @ 59.999Hz display, not the panel it was seen on. Still open,
still unattributed.

### Closeout verification

- `main` at `883d216`, level with `origin/main`, **0 unpushed commits**.
- **Working tree clean** (`git status --porcelain` empty); stash empty, so the
  control-binary A/B stranded nothing.
- **CI run 88 green on `883d216`**, including the `--renderer-selftest=d3d11`
  assertion. Runs 86 (`e559d07`) and 87 (`90140f9`) green before it.
- **No prerelease published and no tag created.** Tags were not pushed.
- No `Trace.exe`, build, harness or polling process left running.
- Regression for each phase is in that phase's own section. Phase 5's was taken
  on a **1920x1080 @ 59.999Hz display, not the panel**, against a control rebuilt
  and measured on the same display — so it is valid as an A/B and **not**
  comparable to the phase 2–4 tables.
