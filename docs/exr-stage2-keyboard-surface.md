# EXR stage 2, part 2: the keyboard surface

Record of what was built, measured and found. 2026-08-24, physical panel
5120x1440 @ 239.999Hz. Commits `560b5ea` (the collision checker), `c1047cc`
(`C`), `bac8755` (`[` and `]`), `3938223` (the harness), `f1ec025` (code motion),
on branch `exr-stage0-dependencies`. **Not merged — the merge is the owner's.**

Scope was **input and shortcut behaviour only**, by instruction. What is
deliberately NOT here: the transient pass overlay, the View-menu pass list,
`ExrPass::duplicateOf`, and every deeper multilayer grouping or rendering
change. Two `Previous Pass` / `Next Pass` menu rows exist, because a shortcut
with no menu presence is undiscoverable and the actions must be window-hoisted
anyway; that is the discoverable half of the keyboard surface, not the pass list.

---

## The display, checked first

`refresh.ps1` reads the active path as **5120x1440 @ 239999/1000 = 239.999Hz**,
and `Get-CimInstance Win32_VideoController` shows the RTX 4090 carrying that mode
with both virtual adapters holding none. The panel had returned to its recorded
mode on its own between sessions; nothing was changed.

Corroborated from inside the binary, which is stronger than either API: the HUD
reads `scr Odyssey G95SC` at `dpr 1.00`, and its own stalls threshold prints as
**`(>8.3ms)`** — computed as two refresh periods, so the running build agrees it
is on a ~240Hz display.

---

## The decision that shapes everything: all three keys are QActions

`ShortcutTable::dispatch()` **matches on the key and ignores modifiers.** Its own
header says so, and Shift+Right stepping a frame is that rule working. The
consequence had never been checked: a table-dispatched row for `C` also fires on
Ctrl+C, and the only thing preventing that is whether Qt's shortcut map consumed
Ctrl+C first.

On a QAction, `C` and `Ctrl+C` are two distinct sequences Qt resolves properly,
so the collision **cannot exist** rather than being masked. Three further
properties come with that choice and none of them had to be built:

- **The menu-bar case is correct by construction.** Qt runs an action's shortcut
  in the shortcut map *before* `QMenuBar::keyPressEvent` sees the key. That is
  exactly why bare `H` was never reproducible in the 2026-08-21 bare-letter bug
  while `F`, `S`, `E` and `T` — table rows, which the menu bar reaches first —
  all were.
- **A disabled QAction declines its own shortcut**, so `[` and `]` fall through
  harmlessly on video, audio, a one-pass still and an empty window, instead of
  reaching a handler that has to check and refuse.
- The menu row, the key and the accessible name are one action, which is the
  spec's shared-action requirement.

---

## `warnOnShortcutCollisions()`, and what it found on its first run

A new startup check walking `shortcuts_`, reporting two classes: two bindings on
the identical sequence, and a **bare table row whose key is the key half of a
modifier'd QAction shortcut**. A warning rather than an assert — the collisions
it finds are usually harmless; the value is that the next bare key cannot
introduce one silently.

It earned itself immediately, like the mnemonic check before it, and **the
finding is pre-existing**:

```
trace-keys: MASKED COLLISION -- "Fast-forward - 1x, 2x, 5x, 10x, 30x" is
dispatched on bare L by ShortcutTable, which ignores modifiers, so it would also
fire on Ctrl+L ("Rotate Left"). Safe only while Qt's shortcut map consumes
Ctrl+L first.
```

**Measured harmless in practice**: Ctrl+L on a paused clip reads
`Paused | frame 0 | speed 0.00x | Rotate Left | Frame: 0` — the rotation ran and
`speed 0.00x` says no shuttle started.

**Binding `C` and the brackets added no new line**, which is the design working;
and the checker is not silent in general, since it still reports the `L` row.

---

## What `[` and `]` actually do

A pass change is a different set of **channels** read from the same file, so it
is a reload rather than a re-map. Every mechanism was already built in part 1 and
this is the fourth:

| already existed | what it does |
|---|---|
| `StillImageLoader::setPreferredPass()` | the choice, on the loader because every frame of a sequence must read one pass |
| `frameCache_.clear()` | `loadCurrentFrame`'s cache-hit branch already carried the comment *"a pass change clears the cache, so they cannot be stale"* — this is the caller that makes it true |
| `syncDisplayMapForActivePass()` | already called on both load branches |

The cycle **wraps**; stopping at the ends would put the last pass of a nine-pass
file four keystrokes from the first for no reason. The message names the pass
that **loaded**, never the one that was asked for — `choosePass()` falls back
rather than failing when a frame lacks the requested pass, so reading the request
back would announce a pass that is not on screen.

**The empty layer name round-trips, and it looks like it should not.** Cycling to
the root pass sets the preference to `""`, which `choosePass()` reads as "the
file decides" rather than as a match — but its first fallback is the root colour
group, which is the pass being asked for. The ambiguous case can only arise on a
file with no root pass, and on such a file no pass has an empty layer name, so
`""` is never what gets set.

---

## THE BUG: the enable state was the speed menu's bug again

Gated only in `syncMediaDependentActions()`, the pass actions were computed from
an **empty pass list on every open**, and nothing re-ran them. `openPath()` calls
that function before `loadCurrentFrame()` writes `currentImage_`.

**Measured through UI Automation rather than judged from a screenshot** — the
recorded lesson that a greyed menu row cannot be read from pixels (phase 8's
menu-icon luminance accused a correct build that way):

```
Color Transform         IsEnabled = True
Previous Pass           IsEnabled = False      <- on a 9-pass EXR
Next Pass               IsEnabled = False
```

…while the same window's HUD read `pass 1/9`. Synced from `refreshHud()` now,
beside `syncPlaybackSpeedActions()` and for its reason: it runs after every path,
and it sits **above the `showHud` early return** so the shipping HUD-hidden
configuration is covered. `setEnabled` is a no-op when nothing changed.

This is the same failure as the 2026-08-21 speed-menu defect — state synced at
the sites someone remembered rather than at the one place every path passes
through — and it gets the same answer.

---

## Measured behaviour: `scripts/measure/passkeys.ps1`, seven legs, all PASS

Physical panel, `d3d11`, scratch `TRACE_SETTINGS_FILE`, 9-pass Redshift EXR
(`icecream_passes%04d.exr`, 27 channels, DWAA) and the 4K H.264 clip.

| leg | result |
|---|---|
| `]` advances | **0 of 10 presses changed nothing** |
| `]` wraps | **returned to the opening pass after exactly 9 presses** on a 9-pass file |
| `[` undoes `]` | next moved 18.311%, next-then-prev **0.086%** from start (media line byte-identical) |
| menubar `[` | popups **0 → 0**, hud moved 17.319% |
| menubar `]` | popups **0 → 0**, hud moved 17.319% |
| menubar `c` | popups **0 → 0**, hud moved 10.486% |
| Go to Frame (spin box) | hud moved **0%** across `c [ ] hjkltefsm` |
| control: `[` outside | hud moved 17.506% |
| **Go to Timecode (QLineEdit), LUT loaded** | picture moved **0%** across `c [ ] hjkltefsm`; field reads **`c[]hjkltefsm`** |
| control: `c` outside | picture moved **99.946%** |
| Ctrl+C copies | clipboard holds an image |
| Ctrl+C leaves the picture | **0%** |
| Ctrl+L still rotates | picture moved 100%, `speed 0.00x`, `Rotate Left` |
| `[` `]` inert on video | picture moved **0%** over three presses |
| `C` toggles | picture moved **95.849%** |
| `C` toggles back | **0%** from the starting state after two presses |

**`barekeys.ps1` still PASS** — no bare key opens a menu in either focus state,
so the pre-existing letter surface is unregressed by the new bindings.

### Two text-field legs, because one of them cannot prove what it looks like it proves

**Go to Frame is a `QInputDialog` spin box**, whose validator rejects letters
outright. That leg shows the keys did not *act* and nothing more — and it runs
without a LUT, so a `c` that leaked would have been invisible.

**Go to Timecode is the real `QLineEdit` case** and is the one that matters for
`C`, which is *enabled* on video: a guard that leaked would toggle the LUT while
the user typed. Run with a LUT loaded so a leak would be a 96%-of-the-picture
event. The field reads **`c[]hjkltefsm`** — all three new keys plus the nine
letters phase 7 proved — while the HUD behind still reads `xform ON
ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube` and the picture moved 0%.

It needs media that **has** source timecode (Go to Timecode is disabled
otherwise), so it runs on the ProRes 4444 clip; an EXR sequence carries no
container timecode at all.

---

## Reading `C` on an EXR: there is no gap, and the first reading of this was wrong

The video HUD's `xform` field is built inside the video branch, so an EXR
sequence never reaches it — which looked like a hole that would make `C`
unverifiable on the media it is for. It is not. The EXR media line reports the
stage through the **`map`** field:

| state | EXR media line |
|---|---|
| transform ON | `… \| pass 1/9 (root) colour [R,G,B] \| map OCIO` |
| transform bypassed | `… \| pass 1/9 (root) colour [R,G,B] \| map Gamma 2.2 [0.0029..2.5195, 52.1% >1]` |

The mapping in force is always named, which is part 1's stated contract
(*"normalising is allowed, doing it quietly is not"*). **No HUD change was needed
and none was made.**

**What is still true, recorded rather than fixed:** the EXR line does not print
the LUT's *name*, and it cannot distinguish "no transform loaded" from "loaded
but bypassed" — both read `map Gamma 2.2`. On video those are `xform none` and
`xform bypass <lut>`. A one-line addition when the pass overlay lands; out of
scope for an input-only change.

---

## Three harness faults, each of which reported a WORKING build as broken

Recorded because each is a class, and all three were caught by looking at the
crops rather than by trusting the verdict.

1. **SendKeys reserves `[` and `]` and silently swallows them unescaped.** Nine
   of ten presses read as no-ops on a build where all ten worked. Braces escape
   them; `SendKey` does it in one place now.
2. **The HUD band was a fixed offset that fitted video** and sat ~300px above an
   EXR sequence's two-line HUD — it captured picture and the transport strip.
   Bottom-anchored with an explicit per-media depth is the fix, and it is the
   trap `barekeys.ps1` already records from the other direction.
3. **The band then included the transport line, which prints `refreshHud()`'s own
   action label** — `Open file`, then `next pass`, then `previous pass`. So
   identical passes compared as *different*, and the cycle was reported as never
   wrapping. **The harness was reading its own stimulus.**

**`Same` and `Moved` are two thresholds with a gap, not one cutoff.** The
measured populations are 0.000–0.086% and 10–96%; a reading between 0.50 and 2.00
fails rather than being rounded toward the expected answer. The 0.086% is text
antialiasing across a repaint — six sampled pixels of a 22-row band — and a
single 0.05% cutoff called that a difference on crops whose every field matched.

**Every "unchanged" leg carries a negative control**, because a run where
SendKeys went nowhere would otherwise pass by doing nothing at all — which is
precisely how the first version of the text-field leg passed on a build whose
brackets were disabled.

---

## Regression at HEAD, flat

Physical panel 5120x1440 @ 239.999Hz.

- `scrubbar.ps1` full pool **PASS — 22 files, 88 legs, `delta 0` throughout**,
  4.9 min warm. `kf-land` non-zero on exactly the two recorded long-GOP rows
  (Universe leg 2 = 3, WeLo leg 2 = 18) and **0 on every ProRes row**.
- 4K H.264 cadence x2 **100.0 / 100.0%**, 120 frames, `drop 0`, `rephase 0`,
  `tick-late 0 of 119`, `handler>budget 0 of 119` (max 4.6 / 4.5), buckets
  `~1x 119`.
- 4444 cadence x2 **99.8 / 99.8%**, 261 frames, `drop 0`, `rephase 0`,
  `handler>budget 0 of 260` (max 34.3 / 33.8). The `<0.9x` bucket reads 1 / 9,
  inside that file's recorded 1–10 span — not chased.
- Selftests: `renderer=d3d11 fellback=0 planar=1` · `trace-ocio version=2.5.2 …
  moved=1` · `OK - 11 shapes x 4 scale factors`.
- `verify_trace_assets --strict --no-pillow` exit 0 at **33 embedded files**.
- `barekeys.ps1` **PASS** on both focus states.

**The shipping-path cost is one `currentImage_` check and two `setEnabled`
no-ops per `refreshHud()`**, which is what the cadence rows above bound.

---

## Independently revertable, checked rather than asserted

Each of the three feature commits reverts cleanly **and the reverted tree
builds**, tested one at a time.

**The first attempt failed and the fix is recorded**: the pass-cycling
declarations went in directly beneath `warnOnShortcutCollisions()`, which put two
separately-revertable commits on adjacent lines — `git revert` then conflicts on
whichever landed second, not because the changes interact but because git can
only see that they touch. This is the trap phase 14 paid for with Loop and Copy
Current Frame. `f1ec025` moves the declarations into the colour-transform group,
**proven pure code motion**: the two versions of `MainWindow.h` are identical as
a sorted multiset of non-comment lines.

---

## What part 2 still owes

1. **The transient pass overlay** naming the current pass on screen. The existing
   toast is used for now — `showTransientMessage`, the machinery ~35 other sites
   use — because the keyboard surface needed feedback to be testable at all.
2. **The full pass list in the View menu.**
3. **`ExrPass::duplicateOf` is declared and never filled.** The mechanism is
   settled and cheap (read a band of scanlines from both channel ranges at open
   and compare) and it must land **with** the pass list — an unfilled field the
   HUD would print is exactly the kind of thing that quietly never gets done.
4. **The EXR line's colour-stage detail**: the LUT's name, and `none` versus
   `bypass`. One line, with the overlay.
