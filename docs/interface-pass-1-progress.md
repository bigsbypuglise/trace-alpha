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
