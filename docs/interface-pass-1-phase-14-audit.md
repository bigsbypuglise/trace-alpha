# Interface pass 1 — phase 14 audit (2026-08-11)

The handoff asked for this before planning: *"the spec's menu structure names items that do
not exist anywhere in the codebase … report which is which before implementing, the way
phase 1's audit did."*

This is that report. It is **read-only** — nothing was changed to make it true. Session
display: **physical panel, 5120x1440 @ 239.999Hz** (`refresh.ps1`), so it is comparable with
phases 11–13 and not with 5–9.

The conclusion in one line: **the spec's phase 13 ("menus, help and accessibility polish") is
four kinds of work, not one, and only about half of it is menus.**

---

## 1. What the menu structure asks for, and what exists

The spec's Menus section (`docs/interface-pass-1-spec.md` §3, "Menus"):

> **File** — Open File…, Open Recent, Close Media, Exit.
> **Edit** — Copy Current Frame (only if safely supported), Rotate Left, Rotate Right, Flip
> Horizontal, Flip Vertical, Reset View Transform.
> **View** — Enter/Exit Fullscreen, Always on Top, Actual Size, Fit to Window, Zoom In, Zoom
> Out, Playback Speed, Time Display, Loop.
> **Window** — Minimize, Maximize/Restore, Actual Size, Fit to Window, Show/Hide Movie Inspector.
> **Help** — Trace Help, Keyboard Shortcuts, Report an Issue, Check for Updates (only if an
> updater exists), About Trace.

Twenty-five entries. Here is every one of them against the tree.

| item | menu | state today | class |
|---|---|---|---|
| Open File… | File | `openAction_`, Ctrl+O | **A** wiring |
| Open Recent | File | `recentMenu_`, phase 11 | **A** wiring |
| Close Media | File | **nothing** | **B** new behaviour |
| Exit | File | `quitAction`, Ctrl+Q | **A** wiring |
| Copy Current Frame | Edit | **nothing** | **B** new behaviour, and harder than it reads |
| Rotate Left / Right | Edit | phase 10, Ctrl+L / Ctrl+R | **A** wiring |
| Flip Horizontal / Vertical | Edit | phase 10 | **A** wiring |
| Reset View Transform | Edit | phase 10 | **A** wiring |
| Enter/Exit Fullscreen | View | `fullscreenAction_` + `exitFullscreenAction_`, phase 6 | **A** wiring (currently under File) |
| Always on Top | View | **nothing** | **B** new behaviour, with one trap |
| Actual Size | View, Window | **nothing** | **D** renderer work |
| Fit to Window | View, Window | **nothing** (it is the only thing the renderer does) | **D** renderer work |
| Zoom In / Zoom Out | View | **nothing** | **D** renderer work |
| Playback Speed | View | **split** — see §4 | **A** for five rungs, **C** for 0.5× |
| Time Display | View | phase 7, four modes + two Go To | **A** wiring (currently under File) |
| Loop | View | **nothing** | **C** playback-path work |
| Minimize | Window | **nothing** (`showMinimized()` exists in Qt) | **B**, trivial |
| Maximize/Restore | Window | **nothing** as a command; the *state* is tracked | **B**, trivial |
| Show/Hide Movie Inspector | Window | `inspectorAction_`, Ctrl+I, phase 13 | **A** wiring |
| Trace Help | Help | **nothing**, and no destination exists | **B** + an owner decision |
| Keyboard Shortcuts | Help | **nothing** — but `ShortcutTable::rows()` is complete | **A**, and it is the phase's headline |
| Report an Issue | Help | **nothing**, and no destination exists | **owner decision first** |
| Check for Updates | Help | **nothing**, and no updater exists | **omit** — the spec says "only if an updater exists" |
| About Trace | Help | **nothing**; no version reaches the app | **B**, trivial once a version is plumbed |

The handoff's grep result reproduces exactly: `zoom`, `actualSize`, `fitToWindow`,
`alwaysOnTop`, `Loop`, `Copy Current Frame`, `Minimize`, `Maximize/Restore` and `Close Media`
appear nowhere in `src/` outside comments and unrelated `QEventLoop` hits.

**The four classes:**

- **A — menu wiring.** The behaviour exists and is already a shared `QAction`. Adding it to a
  menu is an `addAction` call. Zero risk to playback.
- **B — net-new behaviour, self-contained.** Real code, but it does not touch the decoder,
  the renderer or the playback clock.
- **C — playback-path work.** Touches the tick, the audio master clock, or the rate machine.
  Priority 1 applies directly.
- **D — renderer work on a signed-off path.** Changes the fit, and therefore the scrub preview
  size and cache depth (§22.8). Priority 1 applies with force.

---

## 2. Class A is most of the menu, and it is cheap

Fifteen of the twenty-five entries are class A. Nine of those are already in a menu and simply
move: Time Display and Share are under **File** because phase 7 and phase 8 put them where
they were reachable rather than where they belong, with a comment saying phase 14 would
restructure. Fullscreen and the HUD toggle are under File for the same reason.

**The Keyboard Shortcuts window is class A, and that is the whole point of phase 3.**
`ShortcutTable::rows()` is complete: 20 rows, grouped `File / Playback / Stepping / Shuttle /
View`, with action-owned rows pointing *at* their `QAction` rather than copying its keys. The
window renders `rows()` and nothing is hand-written. It is a `QDialog` with a table in it —
genuinely a day's work, and the reason the table was built two months before there was
anything to render it into.

One thing to check when it is built: three view-transform actions (Flip H, Flip V, Reset) have
**no shortcut** and are deliberately absent from the table, because the table is the *keyboard*
contract and a row with no key would be a menu listing. That is correct and should stay — the
Keyboard Shortcuts window must not grow a second source for them.

---

## 3. Class B — four small items and two that are not as small as they look

**Minimize / Maximize/Restore** are one line each (`showMinimized()`, `showMaximized()` /
`showNormal()`). The only care needed is that phase 12's `windowIsNormalShape()` predicate
already treats maximized as an aspect-lock exception, so these inherit the right behaviour.

**About Trace** is trivial *once a version reaches the app*. It does not today:
`CMakeLists.txt:2` declares `project(Trace VERSION 0.1.0)` and nothing passes it through.
`src/main.cpp:254` sets the application *name* and no version. One compile definition.

**Always on Top has a trap that is invisible from the menu spec.** The obvious implementation
is `setWindowFlag(Qt::WindowStaysOnTopHint)`. On Windows, changing a top-level widget's window
flags makes Qt **destroy and recreate the native window** — and the D3D11 swapchain's surface
is a child HWND created once in `initialize()` from `host->winId()`
(`D3D11VideoRenderer.cpp:463`). Recreating the parent orphans it. The safe implementation is
`SetWindowPos(hwnd, HWND_TOPMOST | HWND_NOTOPMOST, …)` on the top-level HWND, which changes
nothing about the window's identity — and `MainWindow` already has a `nativeEvent` override
from phase 12, so the Win32 plumbing is there. The state also has to persist, through
`trace::app::settings()` and nowhere else (phase 11's rule).

**Close Media is a new *state*, not a new command.** `openPath()` (`MainWindow.cpp:3227`)
already contains the complete teardown — 40 lines of it, ending in `videoDecoder_.close()`
and `frameSource_.reset()` — but Trace has never had "media was open and now nothing is". The
things that would have to answer for an empty state: the media-shaped window (§4 asks the
media for an aspect ratio), the transport and its timeline, the HUD, the Movie Inspector
(which already prints *"No media open."* — that path exists), and the Share gate. Modest, but
it is a state audit rather than a menu entry.

**Copy Current Frame is the one the spec itself hedges** — *"only if safely supported"* — and
on the shipping path it is not a clipboard one-liner. Since GATE C, a full-resolution frame on
`d3d11` is **three YUV planes**, and `VideoFrame::toQImage()` returns a **null QImage** for
them by construction: `qtFormatFor()` refuses planar layouts on purpose
(`VideoFrame.cpp:52-62`), so that a plane never gets shown as garbage. Copying the current
frame therefore needs an explicit full-res YUV→RGB conversion at copy time, with the right
colour matrix — which is exactly the work GATE C moved off the CPU and into the shader. It is
a decoder-side call, not a viewer-side one, and it also has to refuse a preview-resolution
frame (mid-drag) rather than silently copying a soft one.

None of that makes it hard. It makes it *not free*, and the spec's own hedge is the right
reading.

---

## 4. Class C — Loop and 0.5× are playback-path work

**Playback Speed splits.** The spec asks for `0.5× / Normal — 1× / 2× / 5× / 10× / 30×` with
the checked item reflecting the effective rate.

- **1× / 2× / 5× / 10× / 30× are class A.** `PlaybackController`'s ladder is literally those
  five rungs (`kShuttleLadder`), `startShuttle()` is the shared entry point phase 3 extracted,
  and `playback_.state().speed` is the one source of truth the checked state reads. This is a
  menu over machinery that already exists.
- **0.5× does not exist at any level.** It is not in the ladder, not in the controller, not in
  the scheduler. Half-speed forward means presenting each source frame for two frame periods —
  and **audio is 1× forward only** by an explicit decision ("half-working sound is worse than
  none in a review tool"), so 0.5× is either silent or needs a resampler. Either way it is a
  change to the audio-mastered playback path, which is priority 1's core.

**Loop is small but it is in the tick.** Playback currently *stops* at the last frame and
`playbackAtEnd_` makes the next Play rewind (`c3335ec`). Loop means the tick wraps instead of
stopping, and the audio has to restart at the wrap point — `startAudioForPlayback()` takes its
offset from the current frame, so a wrap without it would run picture against stale sound.
Not large. But it is the playback clock, and every phase of this pass runs the cadence
regression for a reason.

---

## 5. Class D — the view-scaling group, and one correction to the handoff

**Actual Size, Fit to Window, Zoom In and Zoom Out are one feature, and it is renderer work.**

`trace::render::fitDeviceRect()` (`VideoRenderer.cpp:43`) is the **single** fit expression both
backends share — five lines, `Qt::KeepAspectRatio`, always centred, always fit-to-window.
There is no scale term and no pan origin anywhere in `src/render/`. "Fit to Window" is not a
command Trace lacks; it is the only thing Trace does.

Three consequences, of which the handoff named three and one of them has expired:

**(a) It changes the fit, and the fit is load-bearing.** `lastDrawSize` is measured *by* the
paint, the scrub preview converts to it, and cache depth follows it (§22.8). Zooming to 4:1 on
4K media makes every preview four times the area. This is priority 1, directly.

**(b) The reduction-taps concern is ALREADY ANSWERED IN CODE — ninth premise-expiry.** The
handoff says *"step 9 solved downscaling … zooming in is upscaling, which it never covered,
and `taps` collapsing to 1 there is a separate decision rather than an automatic answer."*
`updateReduction()` (`D3D11VideoRenderer.cpp:823`) already gates on
`fitted.width() < content.width() && fitted.height() < content.height()` and carries the
reasoning verbatim: *"Only when reducing. Upscaling has no undersampling to fix … a box
average there would blur a magnified frame, which is the opposite of what a review tool wants
when someone is inspecting pixels."* Step 9 covered it. **Nothing is open here.**

What *is* open, and is a different question the handoff did not ask: **above 1:1 the D3D11
sampler is bilinear**, so a magnified frame is smoothed. A review tool at 4:1 probably wants
**nearest** — pixels as pixels. The CPU path already has the shape of this rule (it draws
unfiltered at exactly 1:1, `TRACE_NEAREST_SCALE` is its control). So the magnification filter
is a real owner-facing picture decision on a path signed off at step 9, and it is exactly the
kind of thing that should not be decided quietly inside a phase called polish.

**(c) §4 assumes the picture fills the viewport with no bars.** Actual Size on 4K media inside
a 1280x720-equivalent capped window puts the picture **larger than the viewport** — which needs
panning or cropping, a model Trace has never had. That interacts with
`View ▸ Lock Window to Media Aspect Ratio`, with §4's maximize/snap/fullscreen exceptions, and
with the window-shape code phase 12 signed off eight days ago.

**Recommendation: this group is its own phase with its own regression**, not a line item inside
phase 14.

---

## 6. Accessibility — construction, and it is executable here

`grep -rn "QAccessible\|accessibleName" src/` still returns **nothing**. The position is
unchanged from the phase 6 record: the menus are real `QMenu`s, `TRACE_TRANSPORT_BAR=1`
restores real widgets, every command has a shortcut and `ShortcutTable::rows()` enumerates
them — but the **floating transport, which is what ships, has no widget tree and exposes
nothing.**

The design is plan §19.7 and it is a plan, never prototyped: input-transparent zero-painting
proxy widgets on the same rects `OverlayModel` lays out, names and shortcuts read from the
`QAction`s the hooks already call, checked/disabled read from the same actions, a real tab
chain.

**The geometry must come from `OverlayModel`, not be re-derived.** It already owns layout and
hit-testing (`dPanel_`, `dPlay_`, `dRewind_`, `dFfwd_`, `dShare_`, `dTrack_`, and the `Region`
enum that makes "what is under the pointer" and "what was pressed" one answer). A proxy tree
built from a second set of rects drifts, and drifts silently.

**And it is drivable.** Narrator ships with Windows and is on this machine. That makes §31.5
item 4 — *"the overlay is not final until a screen reader has driven one"* — categorically
different from mixed-monitor DPI, which is untested for want of hardware and is tabled. This
one is untested only because nobody has run it. It should be **run and what it announced
recorded**, not asserted.

---

## 7. The split — OWNER DECISIONS TAKEN, 2026-08-11

The split was recommended and **accepted**. Everything after phase 14 shifts by one.

**Phase 14 — menus, help, accessibility, and three items the owner routed into it.** The full
File/Edit/View/Window/Help structure; the Keyboard Shortcuts window from `rows()`; Trace Help /
Report an Issue / About Trace; Minimize, Maximize/Restore, Always on Top, Close Media; the
Playback Speed menu; **Loop**; **0.5×**; **Copy Current Frame**; the Movie Inspector's
**Duration** row; and the accessibility proxy tree driven by Narrator.

**Phase 15 — view scaling.** Actual Size, Fit to Window, Zoom In, Zoom Out, the pan model they
imply, the magnification-filter decision, and their interaction with §4's aspect lock. Its own
regression, because it moves the fit.

**Phase 16 — full regression.** Closes the interface pass.

### The rulings, each recorded at its stated width

- **Check for Updates is OMITTED.** No updater exists and the spec conditions the item on one.
  Not a greyed row, not a stub — absent, the same honesty as phase 13's one-item Window menu.
- **Report an Issue is a pre-filled `mailto:` to `bigsbypuglise@gmail.com`**, carrying the
  Trace version and build in the body. Chosen over the GitHub repo because the repo is private
  and a link there is a dead end for any tester who is not the owner.
- **The Duration row is ADDED**, in the inspector's General section, origin **`encoded`** —
  it is the container's claim, not something Trace measured. This closes the phase 13
  discrepancy where it was found rather than carrying it further.
- **Loop, 0.5× and Copy Current Frame are all IN phase 14.** The audit recommended deferring
  all three as playback-path or decoder-side work; the owner overrode that and took them.
  Recorded as a decision, not an oversight — the concern was raised, read and answered.
  Consequence: **phase 14 contains playback-clock work and a decoder-side conversion**, so
  priority 1 binds it as hard as any engine phase, and the cadence regression is not a
  formality here.

**0.5× and audio needs no new decision**, and this is worth stating so it is not taken as one.
The standing rule is that audio is 1× forward only, enforced by a single guard in the tick that
catches every way playback stops being 1× forward. 0.5× is caught by that guard by
construction, so **0.5× is silent** — the existing rule applied, not a new exception. A
resampler is separate work and is not in this phase.

`Ctrl+0` remains unclaimed and spoken for twice — the approved package wants it for Reset View
Transform, the spec for Actual Size. **Whoever adds Actual Size takes it**, which under this
split is **phase 15**, not phase 14. Unchanged since phase 10.
