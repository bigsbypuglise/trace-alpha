# The interface pass is open, PHASE 14 IS BUILT AND MEASURED, and PHASE 15 IS NEXT.

**PHASE 14 SHIPPED (2026-08-11): the full menu structure, a generated Keyboard Shortcuts
window, Help, and an accessibility proxy tree over the composited transport that has been
DRIVEN rather than asserted.** Full record in `docs/interface-pass-1-progress.md` under
"Phase 14"; the audit is `docs/interface-pass-1-phase-14-audit.md`.

**NOTHING IN PHASE 14 IS SIGNED OFF.** Everything is measured; **owner visual and behavioural
testing of Loop, 0.5×, Copy Current Frame, the menus, Help and the Narrator listen all remain
PENDING.** Do not read any figure below as an acceptance.

**THE THREE PLAYBACK-PATH / DECODER FEATURES ARE EACH IN THEIR OWN COMMIT, on owner
instruction**, so any of them can be reverted without taking the menu structure with it:

| commit | contents |
|---|---|
| `78bc67b` | the audit |
| `bccd21e` | menus, Keyboard Shortcuts window, Help, Minimize/Maximize/Always on Top/Close Media, mnemonic guard, the disabled-action fix |
| `eeea986` | the accessibility proxy tree (Space-focus regression folded in, not shipped and fixed) |
| **`d9a4840`** | **Loop** |
| **`a218643`** | **0.5×** |
| **`5de3552`** | **Copy Current Frame** |
| `a78dedc` | harness |
| `f603e22` | gitignore |

The first cut bundled all three features into the menu commit. **That was the error to avoid
repeating**: a contested item goes in its own commit from the outset, not after someone asks.

**"Independently revertable" was CHECKED, not assumed, and the first attempt failed.**
`git revert` on the Loop commit conflicted in two places — not because the features interact,
but because Loop's one-line `setEnabled` sat directly beside Copy Current Frame's, and
`loopWrap()` sat directly beside `copyCurrentFrame()`. **Two independent one-line additions on
adjacent lines conflict on whichever is reverted second**, because git can only see that they
touch. Fixed by pure code motion — the pre-existing `speedActions_` loop now separates the two
one-liners, and `copyCurrentFrame()` moved past `syncMediaDependentActions()` — verified as a
reordering by sorting both files and diffing (identical as a multiset of lines, plus six
comment lines). **All three now revert cleanly AND the reverted tree builds**, checked for each.

The restructure changed no behaviour: the only difference from the tag on the measured build is
that code motion and its comments.

---

## What phase 14 changed, and what is open because of it

**THE PHASE WAS SPLIT AND THE OWNER ACCEPTED THE SPLIT.** The spec's "menus, help and
accessibility polish" names twenty-five entries and they audit as four kinds of work: fifteen
are **wiring** over shared `QAction`s, four are **small net-new behaviour**, two are
**playback-path work**, and four — Actual Size, Fit to Window, Zoom In, Zoom Out — are
**renderer work**. So **phase 15 is view scaling, with its own regression, and phase 16 is the
full regression that closes the pass.**

Owner rulings, all taken 2026-08-11: **Check for Updates OMITTED** (the spec conditions it on
an updater and none exists); **Report an Issue is a pre-filled `mailto:` to
`bigsbypuglise@gmail.com`** rather than a link into a private repo; **the inspector's Duration
row is ADDED**; and **Loop, 0.5× and Copy Current Frame were taken INTO phase 14** against the
audit's recommendation to defer them.

---

## PHASE 15 IS THE OPEN PHASE: view scaling

**Actual Size, Fit to Window, Zoom In, Zoom Out — and the pan model they imply.** Read the
phase 14 audit's §5 before planning it; three things are already established.

**`trace::render::fitDeviceRect()` (`VideoRenderer.cpp:43`) is the SINGLE fit expression both
backends share** — five lines, `Qt::KeepAspectRatio`, always centred. There is no scale term
and no pan origin anywhere in `src/render/`. "Fit to Window" is not a command Trace lacks; it
is the only thing Trace does. Adding a scale and an origin there is the whole mechanism, and it
is renderer-neutral by construction the way phase 10's transform was.

**Priority 1 applies with unusual force.** The fit is what `lastDrawSize` reports, what the
scrub preview size follows, and therefore what cache depth follows (§22.8). Zooming to 4:1 on
4K media makes every preview four times the area. **Expect the scrub baselines to move for a
real reason** — as they did at phase 12 — rather than treating a moved number as a regression,
and quote `display` **and** `win WxH` on every figure.

**THE REDUCTION-TAPS CONCERN IS ALREADY ANSWERED IN SHIPPING CODE — do not re-derive it.**
`updateReduction()` (`D3D11VideoRenderer.cpp:823`) gates on
`fitted.width() < content.width() && fitted.height() < content.height()` and carries the
reasoning verbatim: *"Only when reducing. Upscaling has no undersampling to fix … a box average
there would blur a magnified frame, which is the opposite of what a review tool wants when
someone is inspecting pixels."* Step 9 covered it. **Ninth premise-expiry.**

**What IS open is the magnification filter above 1:1, and it needs an owner decision.** The
D3D11 sampler is bilinear, so a magnified frame is smoothed. A review tool at 4:1 probably
wants **nearest** — pixels as pixels — and the CPU path already has the shape of that rule (it
draws unfiltered at exactly 1:1; `TRACE_NEAREST_SCALE` is its control). **This is a picture
decision on a path signed off at step 9**, so take it explicitly rather than inside an
implementation.

**And §4 assumes the picture fills the viewport with no bars.** Actual Size on 4K media inside
a 1280x720-equivalent capped window puts the picture *larger* than the viewport, which needs
panning or cropping — a model Trace has never had. That interacts with
`View ▸ Lock Window to Media Aspect Ratio`, with §4's maximize/snap/fullscreen exceptions, and
with geometry the owner signed off eight days ago. **Decide what Actual Size does to the window
before writing the pan.**

**`Ctrl+0` is unclaimed and is spoken for twice** — the approved package wants it for Reset View
Transform, the spec for Actual Size. **Phase 15 takes it with Actual Size**, and Reset stays
shortcut-less unless the owner rules otherwise. Unchanged since phase 10.

---

## Owner items outstanding from phase 14 — ALL OF IT IS PENDING

**Nothing in this phase has been accepted.** These are the open items, and until they are
answered phase 14 is measured work rather than delivered work.

1. **Visual review of the menus and Help.** The structure is the spec's and the mnemonics are
   now guaranteed unique, but nobody has read the menus for wording or order. Trace Help has
   **real content written for this phase** rather than a stub or a second surface onto About —
   it is four paragraphs and it is worth reading, because it is the only prose in the product.
2. **0.5× feel.** It is honest half speed — each source frame presented for two frame periods,
   no frame skipped, silent — and nobody has watched it.
3. **THE SCREEN READER HAS NOT BEEN LISTENED TO.** `uiatree.ps1` proves the transport is
   *exposed*: five controls, correctly named, correctly typed, on the drawn rects, with
   descriptions, and **invokable** (invoking Fast-forward through UI Automation with no focus
   and no click reads `speed 2.00x | FF`). **UIA is the interface Narrator consumes, so an
   element absent from that tree is certainly not announced — but an element present in it can
   still read badly, in the wrong order, or with a name that is nonsense aloud.** §31.5 item 4
   stands. Narrator ships with Windows and is on this box; this needs ten minutes and a person.
4. **Loop's persistence.** It survives a file change and a restart, deliberately — it is a
   review preference, not a property of the media. Say if that is wrong.

---

## Four things phase 14 leaves for anything that touches the interface

- **`warnOnDuplicateMnemonics()` runs at every startup and prints to stderr.** Two items in one
  menu sharing an Alt key makes the key **cycle the highlight instead of activating either**.
  Phase 10 hit it once; phase 14 introduced three and the guard found a **fourth that had been
  shipping since phase 7**. A new menu item that collides now says so.
- **`syncMediaDependentActions()` is the one place a command is gated on media being open.** It
  was missing from the open path for one commit, and **a disabled `QAction` does not report
  being triggered**, so Ctrl+C put nothing on the clipboard and the menu item showed no message
  — which reads exactly like a broken conversion rather than a command that never ran.
- **`ShortcutTable::rows()` now has a reader.** A binding change is visible to users rather than
  only to `grep`. **A row with no key is deliberately not printed**: the table is the KEYBOARD
  contract, and a keyless row would be a menu listing.
- **The accessibility proxies are `Qt::NoFocus`, and that is load-bearing.** Plan §19.7 asks for
  a real tab chain; building one **broke the Space bar** — Qt gives initial focus to the first
  focusable widget, every other transport widget is `NoFocus` by an old rule, so the Rewind
  proxy took it and the first Space on a fresh launch read `speed -2.00x | Reverse Play`.
  Switching it back is not a one-line change: something has to hold focus by default first, and
  that is a decision about the viewer.

---

## Two harness lessons from phase 14 that apply to everything

- **`SetForegroundWindow` FAILS SILENTLY from a background process.** A child PowerShell's own
  terminal window takes focus, so every `SendKeys` goes there and the run reports the feature
  missing. Use `AttachThreadInput` and **read `GetForegroundWindow()` back** — an attempt that
  is only attempted is the fault. Same family as phase 11's `SendKeys "%r"` and phase 13's
  `-Mode hold`.
- **P/Invoke's default is `CharSet.Ansi`.** A managed string handed to any `...W` function is
  marshalled as ANSI and read back as UTF-16, so window titles come out **one character long**
  and a name lookup never matches. "Keyboard Shortcuts window NOT FOUND" was printed three
  times against a build where the window was open on screen. **Sixth stale instrument, and the
  first that is a marshalling convention rather than a reading taken at the wrong moment.**

**And a third, which is about measurement rather than about Win32: A PERSISTED PREFERENCE IS AN
INPUT TO A MEASUREMENT.** Loop is persisted; a wrap re-establishes the playback timeline and
zeroes every cadence counter with it. The first cadence run of the phase read `frames 30 |
elapsed 1.25s` against a 119-frame baseline **with `presented 24.00/24.00 (100.0% real time)`
on the same line**, because Loop had been left on by the loop harness's own first run.
`cadence.ps1` passes a scratch `TRACE_SETTINGS_FILE` now, the way `recentfiles.ps1` already
did. **Read the HUD's `loop` field first if a cadence figure is ever questioned.**

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight, fast,
   smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. ~~**No interface work.**~~ **LIFTED by the owner, 2026-08-10.** Priority 1 is now the binding
   constraint *on this work*: every phase runs the playback and scrub regression.
3. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. Five instances: the drag preview, §15's scrub sampling,
   accelerated reverse, accelerated forward, and phase 4's shuttle-press decision. **Do not
   re-open any of them on picture-quality grounds alone.**

## What has been accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10)** — the engine, at 2×/5×/10×/30× in both directions.

**Phases 6, 12 and 13 are signed off** — the floating transport's feel and identity; the
media-shaped window (with stills and image sequences re-signed-off on the corrected build at
`3a38516`); and the Movie Inspector's contents, wording and origin labels.

**Read each at its stated width.** A later summary that says "the transport is done" has
widened it.

## Deferred, with the conditions attached

- **Step 10, 10-bit display output** — two external gates, both outside the code.
- **Mixed-monitor DPI (§20.4)** — tabled for hardware; `AllScreens` returns one display and
  Parsec replaces it rather than adding one. **Not executable on this box at all.**
- **BT.2020 has no tonemap** (§22.7 item 5). Known gap, never a complaint.
- **GATE E step 2 — vsync snapping** — stopped by owner decision. **Do not start it without a
  specific new cadence complaint.**
- **LucidLink read-ahead.** Two designs measured worse. Not in progress.
- **EXR / image sequences and OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI.

## The phase list

12. **Spec §4, media-driven window size** — DONE, signed off.
13. **Movie Inspector** — DONE, signed off.
14. **Menus, help, accessibility, Loop, 0.5×, Copy Current Frame** — built and measured;
    **NOT signed off. Every owner test is pending.**
15. **View scaling** — Actual Size, Fit to Window, Zoom, pan. **The open phase.**
16. **Full regression.** It can then honestly close the interface pass.

## Priority 1 is the constraint on all of it

Every phase runs the playback and scrub regression, not just the last one. Phase 14's, on the
**physical panel, 5120x1440 @ 239.999Hz**: 4K H.264 cadence ×3 **100.0%** with 120 frames,
`handler>budget 0 of 119` and all 119 gaps in the ~1× bucket; 4444 ×2 **99.8%** at 0 of 260;
`-SnapRelease` `target 120 shown 120 delta 0` full-res planar with **`hitch 0`**; both lifecycle
legs; **25 of 25 transitions**. Case for case with phases 12 and 13.

**Build the control binary in a `git worktree`, not by stashing**, and **verify every swap by
hash** (`swapexe.ps1`). `windeployqt` the control, and copy the `av*`/`sw*` DLLs across.

## The rules this project keeps re-learning

**A validated PREDICTION is not a validated MECHANISM.** Phase 14 is the sharpest instance yet
in a new direction: plan §19.7's proxy tree had been written down and cited for eight phases,
and building it exactly as described **broke the Space bar** — the plan was right about the
mechanism and wrong about the consequence, and only executing it showed which.

**A deferred item's premise expires. Re-derive it before building it.** Nine instances now, the
latest being the reduction-taps concern that step 9 had already answered.

**Check what a number is measured against before believing it.** `frames 30 | elapsed 1.25s`
read `100.0% of real time` on a looping file.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.** Phase 14's
loop control was the test run twice, because Loop is persisted and the first leg saved it.

**A refusal enforced by a comment is a refusal a later change removes.** CLAUDE.md's *"transport
widgets must not take keyboard focus … if a new widget steals arrows/space, this is why"* is a
comment, and it was broken by a widget that was not a `QPushButton`. **Read a rule for what it
protects, not for the class it names.**

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**Reproduce on the reported case AND on a healthy one before theorising.**

## `d3d11` is the default renderer, and two obligations follow

`TRACE_RENDERER=cpu` is the control and the escape hatch — **the first thing to try if anything
about the picture looks wrong** — but it is now the *softer* picture as well as the slower one,
since step 9 only fixed the GPU path.

**Every scrub and playback baseline taken before 2026-08-10 was on `cpu`** and most are not
tagged with a renderer. Valid as records; **not** valid as comparisons against a run taken
today.

## Parsec — ask which display a session is on before comparing any number

Remote sessions present **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @ 239.999Hz**.
`scripts/measure/refresh.ps1` reports the current one — **run it at the START of a session and
again before quoting anything.** `stalls` is `2 × refresh`, so its bar moves 33.3ms → 8.3ms and
**no stall figure crosses that boundary.** `hitch` does. **No subjective smoothness, cadence or
picture-quality judgement is valid over Parsec at all.**

## Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either

`stalls` counts paint gaps over `2 × refresh`, so the same run reads `stalls 51 … | hitch 3`.
**`hitch` is a fixed 33ms bar and is the only stall figure comparable across sessions.** Cache
depth follows the *video rect*, not the window, and `H` changes one without the other.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token.
- **CI asserts the renderer initializes** and **the window-shape geometry across DPI**.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop a
  running `Trace.exe` first** or the link fails with LNK1104.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§`. Use `cat` from the Bash tool. **A `git commit -m` here-string containing `>` or `->`
  fails** — write the message to a file and use `git commit -F`. **Do not pipe a measurement
  script through `Select-Object -First N`**: that raises `StopUpstreamCommandsException` and
  terminates the script mid-run, which looks exactly like the script crashing.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- Harness, **and note which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames**; the **412-frame** clip for `-LadderOut`), `shuttleland.ps1`,
  `scrub.ps1` (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run both
  legs**), `previewshot.ps1`. Note these take no `-Clip`: **`restart.ps1` first**.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**passes a scratch `TRACE_SETTINGS_FILE` since phase 14**),
  `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `sidebyside.ps1`, `stalls_vs_window.ps1`,
  `abfilter.ps1`/`croprect.ps1`, `recentfiles.ps1`, `inspector.ps1`, `swapexe.ps1`,
  `make_timecode_fixtures.ps1`, `make_shape_fixtures.ps1`, `resizecache.ps1`, and **new at
  phase 14: `uiatree.ps1`** (the UIA tree a screen reader reads — **run it under
  `TRACE_TRANSPORT_BAR=1` as the negative control**), **`phase14.ps1`** (`speed`/`loop`/`copy`/
  `close`/`shortcuts`) and **`menushot.ps1`**.

  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the window
  at native resolution (`capture.ps1`), and **check nothing overlapped it**. A diagnostic
  screen-grab must cover the **whole virtual desktop** — a 1920x1200 grab of the top-left
  corner of this panel contains desktop icons and no application.
- Update `CLAUDE.md` and the plans at the end of the session.
