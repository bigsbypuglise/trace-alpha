# Owner interface feedback, 2026-08-18 — triaged

Eighteen items from the owner's first full pass over the redesigned interface. Grouped by what
the work actually is, not by his numbering, with his numbers kept. **Three have a confirmed
diagnosis in the code and are named below rather than needing investigation.**

Four items reverse or conflict with an earlier decision. Those are flagged, because the point
is that the owner is deciding again with the thing in front of him — not that anything drifted.

---

## Confirmed diagnoses — the cause is known, not suspected

### 18. The console window — NOT a portability artifact, a one-line build flag

`app/CMakeLists.txt:144` reads `add_executable(Trace ...)` with **no `WIN32`**, so Trace links
as a *console-subsystem* executable and Windows creates a console for it on every launch. It
has nothing to do with the portable ZIP. Adding `WIN32` makes it a GUI-subsystem app and the
console never appears — no need to minimise it.

**One consequence to handle in the same change.** The diagnostic logs write to stderr with
`fprintf` — `TRACE_OPEN_LOG`, `TRACE_SHAPE_LOG`, `TRACE_SETTINGS_LOG`, `TRACE_SEEK_LOG`,
`TRACE_LUCID_LOG`, `TRACE_THEME_LOG` — and a GUI-subsystem process has no console to write to
unless one is attached. The harnesses read the HUD rather than stderr, so nothing measured
breaks, but those knobs go quiet. The usual fix is `AttachConsole(ATTACH_PARENT_PROCESS)` at
startup when launched from a terminal, or allocating a console only when a `TRACE_*_LOG` is
set. Decide which; do not silently lose the logs.

**The messages he saw are all benign and worth telling him so:**

- `Application has requested 32 threads. Using a thread count greater than 16 is not
  recommended.` — this is Trace's own **deliberate, measured** intra-only thread policy
  (`av_cpu_count()`, knee measured at 32 on this box: `dec` 62.29ms at 16 → 45.95 at 32). The
  warning is FFmpeg's generic advice, not a problem.
- `Referenced QT chapter track not found` — a benign container note from the MOV demuxer.
- `qt.multimedia.ffmpeg: Using Qt multimedia with FFmpeg version 7.1.2` — **Qt Multimedia's own
  bundled FFmpeg on the audio path**, separate from the FFmpeg Trace links for video. Worth
  knowing it exists; not a fault.

### 13. Clicking the video does not activate the window

`D3D11VideoRenderer.cpp:139–146` handles `WM_MOUSEACTIVATE` and returns **`MA_NOACTIVATE`**.
That is why clicking the picture does not bring Trace forward while clicking the top strip does
— the strip is an ordinary Qt widget and activates normally.

It was deliberate: the surface returns `MA_NOACTIVATE` so clicking the video cannot pull
keyboard focus away from Qt, which is what keeps Space, the arrows and J-K-L working after a
click on the picture. **So the fix is not simply to return `MA_ACTIVATE`** — that would restore
activation and risk the focus behaviour the original choice protects. The shape that gives both
is to activate the *top-level window* explicitly on that message and still decline focus for the
child surface. Verify afterwards that a click on the picture, followed immediately by Space,
still toggles playback — that is the regression this guard exists to prevent.

### 4. The window recentres when new media opens

`MainWindow.cpp:4454` — `target.moveCenter(work.center())`. Centring is **§4's own instruction**
(*"Center the resized window within the current monitor's available work area"*) and was part of
the phase 12 geometry sign-off, so this is a deliberate behaviour the owner is now reversing
having lived with it.

The requested behaviour — keep the position, change only the size — is better and is what most
players do. Anchor the window's **top-left** rather than its centre (predictable, and a
centre-anchored resize moves the title bar under the cursor), clamp so the result stays inside
the same monitor's work area, and keep centring for the **first** open of a session where there
is no prior position to preserve. **Record it as superseding §4 item 7 rather than editing the
constant quietly.**

---

## Bugs to find — one hypothesis offered, do not build on it

### 3 + 7. Controls fade in and out with the cursor stationary

**These are one bug, and the owner is right that they are related.** The recorded behaviour is
the opposite of what he sees: the auto-hide *holds* while `hover_` is a region, so a stationary
pointer inside the client should keep the chrome up indefinitely — measured still up at 6s at
the centre and in a corner. A cycle means something is calling `reveal()` repeatedly with no
input.

**Instrument it before theorising** — count and attribute the calls to `reveal()` over a
stationary-cursor run. Do not guess from reading; three sessions in a row have had a plausible
reading corrected by a control.

**The leading candidate, offered as a hypothesis only:** the fullscreen cursor-hide is a
feedback loop. Hiding the cursor changes cursor state, which can produce a `WM_MOUSEMOVE` or
`WM_SETCURSOR`, which reveals the chrome, which restarts the idle timer, which hides the cursor
again. That would explain both the cycling *and* why fullscreen is worse, since that is where
the cursor hide runs. Note the cursor is hidden by **two different mechanisms** — `Qt::BlankCursor`
on the CPU path, `SetCursor(nullptr)` answering `WM_SETCURSOR` on D3D11 — so check both
backends; a fix on one may not be a fix on the other.

### 16. The slider thumb pops or artifacts while dragging

Likely a consequence of **17** below: the thumb changes size 13px → 16px while scrubbing, so it
is resampled at a new size mid-drag, and step 5 measured exactly this class — a stretched
gradient column read **12,511 differing pixels** across backends, a 1:1 cell with draw-time
alphas **3,594**, and a 1:1 cell with baked alphas **0**. Anything not drawn 1:1 with baked
alpha diverges and shimmers.

**Do 17 first and re-check 16.** If it persists, the owner's own fallback is the right one: a
plain white dot, drawn as a single baked cell at a fixed size, which cannot resample.

---

## Mechanical — small, low risk

| # | item | note |
|---|---|---|
| **2** | rounded ends on the timeline track | radius = half the track height (2px at 4, 3px at 6) |
| **15** | remove Jump to Beginning / End buttons | **Keep `Home` and `End` bound.** The keys cost nothing and stay useful; only the buttons go. Gives the timeline the width he wants. Reverses the decision taken two sessions ago — record it as the owner deciding again with it on screen. |
| **17** | remove the thumb's scale animation | one constant; likely fixes 16 |
| **14** | drop the open/close confirmations | route through `showTransientMessage` already, so this is choosing which sites emit. **Keep Copy Frame** — he named it as the one that earns its place. Errors and refusals must all still appear. |

### 9. Frame counter jitter — the fix is tabular figures, not only alignment

Proportional digits have different widths, so every digit change reflows the string. **Enable
tabular figures** on the readout font (`QFont::setFeature("tnum")` in Qt 6.7+, or
`QFont::Monospace` letter spacing as a fallback) — Segoe UI Variable carries them. Left-aligning
the value as well stops the whole field shifting when the digit *count* changes, which is his
"50 → 51" case. Do both, and apply them to **both** readouts and the HUD's frame field so they
cannot disagree.

### 10. Menu mnemonic underlines — honour the Windows setting rather than hiding them

Those underlines are Windows keyboard-access cues, and Windows' own default is to show them
only after `Alt` is pressed — controlled by `SPI_GETKEYBOARDCUES`. Qt draws them unconditionally
here. **Trace already has a `QProxyStyle`** (added for the slider's absolute-set behaviour), so
this is a `SH_UnderlineShortcut` case in that style, answered from the system setting.

That is better than hiding them outright: it looks the way he wants by default *and* keeps them
for keyboard and screen-reader users, which is what the accessibility work exists to protect.
**`F` stays the Frames readout shortcut** — unrelated and unaffected.

---

## Conflicts with the design package — the owner should decide knowingly

### 5. The empty-state play mark sits right of centre

**This is the design's intent, not a bug.** The mark's ink bounding box sits **9.5px right of
its canvas centre by design** — a right-pointing triangle is balanced by eye rather than by its
box — and what Trace centres is the canvas, so the ink lands at +10.5px. The package's own
mockup measures **+9.5px**, so Trace is matching the design to within a pixel.

The owner prefers it more centred, which is a legitimate disagreement with the designer.
**Centre the ink instead of the canvas** if he confirms, and record it as an owner override of
the delivered art rather than a correction.

### 6. Loop starts highlighted

Two separate things, and the first may be the whole story:

1. **His settings file is probably poisoned.** Before `b2a901b`, clicking a *disabled* Loop with
   no media wrote `loop=true` — `QAction::toggle()` carries no enablement check where
   `trigger()` does. Check his `trace.ini`. If it says `loop=true`, the state is a leftover of a
   fixed bug, not current behaviour.
2. **If he wants Loop to start off regardless, that reverses a phase 14 sign-off** which
   accepted persistence explicitly, on the reasoning that Loop is *"a review preference, not a
   property of the media."* His argument — that a highlighted Loop implies newly opened media
   will loop — is a good one. Either answer is fine; it needs recording as a decision.

---

## The top bar — three items, one architectural question

**1, 8 and 11 are the same problem seen from three sides, and they cannot all be answered
cheaply.**

### 1. Two top bars

Not a portable-app artifact. It is the **native Windows title bar** plus the **`TopChrome`
strip** built at step 7. Both are expected right now: the roadmap's step 4 says *"Do not remove
the native Windows title bar yet"* and removing it is **step 12, the frameless-window
milestone**, deliberately deferred as the highest-risk item in the whole pass — it adds
`WM_NCHITTEST` and `WM_NCCALCSIZE` to the same `nativeEvent` path that already carries §4's
aspect lock and the DPI reshape, both signed-off geometry.

**His own recommendation is the right interim: drop the Trace icon and wordmark from the
strip.** The title bar already carries the app identity, so that is the actual redundancy;
menus and the filename are not duplicated by anything. Cheap, reversible, and it makes the two
bars read as chrome plus content rather than as a doubled header.

### 8 and 11 — translucency and fade on the top strip

Both are blocked by the same fact, and it is worth being straight about it. The strip is a
**native child window**, and §18.4 measured that *every* native-surface variant loses
translucency — there is nowhere for Qt to blend against, because the video is a separate HWND.
The blur exists precisely because plain translucency was not available. For the same reason a
native strip cannot cross-fade the way the composited bottom bar does; it can only be shown and
hidden.

So there are two honest options:

- **Accept the strip as it is** — opaque, popping on and off — and keep real `QMenu`s on a real
  `QMenuBar`, which is what makes the menus reachable by screen readers *for free* and is why
  step 7 was shaped this way. `uiatree.ps1` finds a MenuBar with five MenuItems on real rects.
- **Composite the strip as quads**, like the bottom bar, which gives translucency and fade
  immediately and matches his preference — at the cost of the accessibility work phase 14 had
  to do by hand for the transport. A middle path exists and is worth costing: draw the bar and
  its labels as quads while keeping a real, hidden `QMenuBar` alive for the accessibility tree
  and `Alt` mnemonics, with real `QMenu` popups on open. That is more work than it sounds and
  should be estimated before it is chosen.

**Do not re-propose Mica or Acrylic for this.** It is measured and recorded: every DWM call
returns `S_OK` and changes rows 1–30 only, because the D3D11 surface covers 100% of the client.
And the durable half is independent of the swapchain — DWM backdrops blur what is behind the
**window**, the design blurs what is behind the **element**. Different effects; no configuration
turns one into the other.

**Note this also supersedes the pending backdrop decision.** If the top strip becomes
translucent-like-the-bottom, `TRACE_STRIP_BACKDROP` is moot. If it stays native, the blur is the
only way to get the design's look. **Answer 8 and 11 before shipping the backdrop.**

---

## No action

**12. Copy Current Frame** — confirmed working by the owner.
