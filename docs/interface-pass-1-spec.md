# Interface pass 1 — approved spec, OPEN

**Status: THIS IS THE OPEN PHASE.** The owner lifted the no-interface-work rule on
2026-08-10 and chose this pass as the next phase. The unblock condition the previous header
named has been met: GATE B, GATE C and GATE E all passed, step 9 shipped, `d3d11` became the
default renderer, and the reverse and forward shuttles were built, measured and signed off.

**Priority 1 is unchanged and is now the binding constraint on this work.** No interface
feature may compromise lightweight, fast, smooth playback. Every phase runs the playback and
scrub regression — not just phase 14 — and a feature that costs smoothness loses. Quote
`hitch` and `win WxH`, never a bare `stalls`.

Author: Anj, 2026-08-09. The spec body in §3 is his and is reproduced as approved. §2 was
re-derived on 2026-08-10 and is no longer the 2026-08-09 text; see its header.

---

## 1. Approved assets

`assets/260807 Trace Media Player Icon/` — 201 files.

```
export/
  CONTENTS.txt, README.txt, TRACE_IMPLEMENTATION_SPEC.txt   <- read these first
  base-ui-icons/   svg/ + png/{1x,2x}      33 icons
  player-icons/    svg/ + png/{1x,1.5x,2x,3x}
  png/{macos,windows}/ , svg/              application icon
```

Base UI set (SVG is the source of truth; PNGs are conveniences):

`audio_volume`, `audio_volume_low`, `audio_volume_muted`, `edit_copy_frame`,
`file_open_recent`, `inspector_movie`, `share_menu`, `share_copy_path`,
`share_copy_lucidlink`, `share_show_in_explorer`, `status_unavailable`,
`transport_play`, `transport_pause`, `transport_scan_forward`, `transport_scan_reverse`,
`ui_check`, `ui_radio_selected`, `ui_disclosure_collapsed`, `ui_disclosure_expanded`,
`view_actual_size`, `view_fit_window`, `view_flip_horizontal`, `view_flip_vertical`,
`view_fullscreen_enter`, `view_fullscreen_exit`, `view_loop_on`, `view_loop_off`,
`view_reset_transform`, `view_rotate_left`, `view_rotate_right`, `view_zoom_in`,
`view_zoom_out`, `window_always_on_top`.

Note `transport_scan_forward` / `transport_scan_reverse` are the continuous-scan artwork
that replaces the current single-frame step buttons. There is deliberately **no**
frame-step icon in the set — stepping becomes keyboard-only.

Existing `assets/icons/` and `assets/Interface/` predate this set. Reconcile or retire them
during phase 2 rather than leaving three icon sources in the tree.

## 2. Dependencies and conflicts — RE-DERIVED 2026-08-10

**This section was rewritten on 2026-08-10 and is not the 2026-08-09 text.** The original
was written before the reverse shuttle, before the forward shuttle, before step 9 and before
`d3d11` became the default, and *a deferred item's premise expires* is the rule this project
has now re-learned six times. Every item below was checked against the code as it stands, at
the file and line given, rather than read forward as still true.

Result: **one item is stale** (2), **one is half-wrong in its premise** (8), **one is
materially larger than written** (1), **two are sharpened by facts that did not exist when
they were written** (6, 7), **one describes a fact that is worse than stated** (9), and the
remaining four are confirmed (3, 4, 5, 10) — though 3 and 5 each need a distinction the
original did not draw.

---

### Item 1 — the composited overlay. STILL TRUE, and BIGGER. Now prerequisite 1.

Everything the original said still holds and was re-confirmed: Qt children over the child
HWND are neither visible nor hit-testable (§18.4/§19), and the transport is still a
laid-out widget *below* the viewer — `MainWindow.cpp:720-734` stacks `viewer_`,
`transportBar_` and the HUD `overlay_` in one `QVBoxLayout`. Nothing floats over anything.

**What changed is which backend the problem belongs to, and the polarity is now inverted.**
The item was written while `cpu` was the default, so a composited overlay read as something
the *opt-in* GPU path would need. `d3d11` became the default on 2026-08-10. So:

- the composited path is what ships to every user, and
- `TRACE_RENDERER=cpu` — the documented escape hatch, *"the first thing to try if anything
  about the picture looks wrong"* — has **no compositor at all**. `CpuImageRenderer::paint`
  (`src/render/CpuImageRenderer.cpp:58-123`) is a `fillRect` plus a `drawImage` and nothing
  else.

A floating transport that lives only in `OverlayCompositor` therefore means **the escape
hatch loses its transport**. That is a new requirement, not a refinement: the overlay needs
a renderer-neutral home reachable by both backends, in the same way and for the same reason
as the view transform in item 6.

Two further findings the original could not have:

- **The spike's hooks carry placeholder *semantics*, not just placeholder art.**
  `installOverlayHooks()` (`MainWindow.cpp:757-791`) wires the overlay's Rewind and
  Fast-forward to `prevFrameAction_` / `nextFrameAction_` — single-frame stepping. The
  transport redesign changes exactly what those two controls mean, so the wiring has to be
  re-pointed, not merely re-skinned.
- **§19.3's cost number does not cover the case that matters.** It is a static overlay held
  visible through one 9s 4K *playback* run. The fade is `QTimer`-driven (`animTimer_`,
  `kFadeMs` 165) and asks the UI thread for repaints; the case to measure is a fade **during
  a drag**, where the UI thread is the contended resource. The metric there is `ui gap`,
  not presented rate.

### Item 2 — reverse shuttle "will likely defer". STALE. The opposite is true.

The engine shipped at `e9fd236` / `dd21fe9`, was measured across four formats and has owner
sign-off. 4K H.264 reverse 1× went **87.0 → 99.2% of real time**; the 1×/2×/5×/10×/30×
ladder runs in both directions; reverse 30× snaps to the keyframe grid at a stable ~15
presents/s. The premise — "continuous reverse is GOP-walk bound" — was itself half wrong:
reverse was **bursty, not slow**, idle 80–93% of the time while missing real time.

So the spec's capability-detection-and-defer branch is **no longer the expected outcome**.
Phase 5 ("reverse shuttle behavior — only if safely supported") is a call site onto
`startShuttleRun(direction, stride)`, which takes any stride. Keep the capability check as a
guard, not as a plan.

**But note what did *not* become free** — see item 3.

### Item 3 — one rate owner with J-K-L. CONFIRMED, and the API it needs does not exist.

`PlaybackController` is still the authoritative mode/speed machine and both ladders live in
it as 1/2/5/10/30 (`PlaybackController.cpp:43-85`). The `userPlayIntent_` warning is still
exactly right: J sets it false (`MainWindow.cpp:3416`), L sets it
`|speed| <= 1.0001` (`:3460`).

Three things to add, all of which decide how phases 3–5 are written.

1. **The button contract and the keyboard contract differ in two transitions, and the
   controller expresses only the keyboard one.** `jogForward()` and `jogReverse()` both
   **enter the ladder at 1×** from a stop (`speed <= 0.0` / `speed >= 0.0` branches), and
   both **reset to 1× on a direction change**. The buttons must start at **2×** in both of
   those cases. That is the owner-confirmed reading, and it is not a call-site tweak that
   pokes `speed` from outside — the smallest honest change is a controller entry that enters
   the ladder at 2×, so there is still exactly one rate machine with two documented ways in.
2. **`startShuttleRun` has exactly two call sites today, `Key_J` and `Key_L`**, and each
   performs a fixed five-step sequence: `endShuttleRun` → controller ladder →
   `prepareVideoRequest` → `beginPlaybackTimeline` → `startShuttleRun`. §29.2 is the
   standing warning here — GATE E was validated on the Play action alone and every other
   path that started the timer kept compiling silently. **Extract that sequence before
   adding a third caller**; do not let a button re-implement it.
3. The run mechanics (`shuttleDir_`, `shuttleStride_`, `shuttleSnapping_`) live in
   `MainWindow` and are derived from `playback_.state().speed`. That is fine and is not a
   second rate machine — but it does mean the button, the menu and J/L must all reach the
   speed through the controller and never through `shuttleStride_`.

### Item 4 — audio during shuttle. CONFIRMED, unchanged, still no work.

`audioShouldDrive()` is still `mode == PlayingForward && |speed| <= 1.0001`
(`MainWindow.cpp:1992`), and the tick still stops audio the moment that stops holding
(`:319-320`). The forward shuttle added in `dd21fe9` is silent above 1× through that same
one guard rather than through a new one. "Provisional safe behavior" remains "keep what
exists", and the policy is already isolated in a single predicate.

### Item 5 — LucidLink detection. CONFIRMED, with a distinction the original did not draw.

`MediaIoSource.cpp:244-257` still classifies per volume by querying it, and is still keyed
on petabyte-scale capacity with `free == total` rather than on drive letter or volume label.
Reuse it; do not write a second one.

**But it answers a storage-class question, not a vendor question.** "Virtual mount with
free == total" is true of any such mount, not only LucidLink. The spec gates *Copy LucidLink
Link* on the path being on a LucidLink filespace **or the shell extension declaring
support** — so this classifier is a good *necessary* condition and a bad *sufficient* one.
The authoritative gate must be the installed integration. Using the classifier alone would
reintroduce, one level up, exactly the "assume all `V:\` paths are LucidLink" mistake the
requirement exists to prevent.

### Item 6 — view-transform contract. STILL TRUE, now cheaper, one new interaction. Prerequisite 2.

`VideoRenderer.h` still has no transform of any kind. Two things have changed since, and
they pull in opposite directions.

**Cheaper:** the two backends no longer compute the destination rect separately. `ddb38ca`
extracted `hostDeviceSize()` and `fitDeviceRect()` into shared free functions
(`VideoRenderer.h:144-145`) precisely because the duplicated arithmetic disagreed at
fractional DPI. So there is now **one** place the fitted rect is produced, and an
orientation transform has one obvious home rather than the two the original item assumed.
D3D11 turns that rect into a viewport (`D3D11VideoRenderer.cpp:903-917`); the CPU backend
turns it into a `QRectF` for `drawImage` (`CpuImageRenderer.cpp:79-82`).

**New interaction, from step 9:** the D3D11 downscale is now a box average whose tap count
is derived from the reduction ratio — `updateReduction(contentSize_, fitted)`, called
per draw. **A 90° rotation swaps which axis is reduced**, so the taps must be computed from
the *post-transform* fit or the filter will average along the wrong axis and step 9's
0.74 → 0.02 result will not hold under rotation. This did not exist when item 6 was written
and is the single easiest thing to get silently wrong in prerequisite 2.

### Item 7 — aspect-locked resize vs the frame cache. TRUE, but the mechanism is not quite as stated.

The concern is real and the debounce is still needed. The detail is worth correcting because
it changes what the fix is.

`resizeEvent` calls `syncScrubPreviewSize()` on every resize (`MainWindow.cpp:985-987`).
Inside it, `reclaimDecoder()` runs **first and unconditionally**, and only then
`setScrubPreviewSize()`. But `setScrubPreviewSize` is itself **guarded**: it returns early
when the size is unchanged (`VideoDecoderFFmpeg.cpp:599`), so the *cache clear* happens on a
real size change and not on every event.

So the two costs are different and only one is what the item describes:

- **cache clear** — only on an actual size change. Harmless for a resize that settles;
  a genuine thrash under continuous aspect-locked drag-resizing, which changes the size on
  every event. The item's conclusion holds for exactly that gesture.
- **`reclaimDecoder()`** — on **every** resize event, changed size or not, which is a
  generation bump per event whatever the size did.

Two fixes, and they are independent: hoist the size comparison above `reclaimDecoder()`
(cheap, correct regardless of aspect lock), and debounce resize-settled before touching the
preview size. Measure both; do not assume either is free.

### Item 8 — zero-based numbering. CONCLUSION RIGHT, PREMISE HALF WRONG.

"No conversion layer needed" is the right answer, and for video the spec's requirement is
**already met, including the part most likely to be missed**: `syncTransportBar` prints
`currentFrame / maxFrame` (`MainWindow.cpp:1065-1067`), so the right endpoint is already the
last valid *index* rather than the count.

But the premise "the HUD works in frame indices from 0" is not true for two of the three
media kinds. The image-sequence and still HUD lines print `st.currentFrame + 1` against a
frame *count* (`:3344` and `:3355`) — one-based. Internal identity is untouched in both
cases, so this is two display strings to change, not a conversion layer, and it must not
turn into one.

### Item 9 — source timecode. TRUE, and the current state is worse than "not extracted".

`VideoMetadata` has no timecode field at all, and `TimeFormat::frameToTimecode(frame, fps)`
synthesises a timecode from the frame index. That is precisely the thing the spec forbids —
*"do not label an elapsed-time conversion as source timecode"* — and the HUD **already does
it**, printing `Timecode: %1` from that synthesised value (`MainWindow.cpp:2762`, also `T`
in `keyPressEvent`).

So this is not only missing extraction work. The existing readout is already non-conforming,
and phase 7 has to decide what it becomes when no source timecode exists: relabel it as
elapsed, or disable it. Real extraction (SMPTE start timecode, drop-frame metadata) is
separate and still real work.

### Item 10 — double-click for fullscreen. CONFIRMED FREE.

`ViewerWidget` implements no mouse handlers whatsoever. The only `MouseButtonDblClick` in
the tree is in `src/ui/OverlaySpike.cpp:31`, the superseded Qt-widget overlay probe that
§18.4 closed. No established gesture to collide with.

---

### What this re-derivation changes about the plan

**Two prerequisites come before any spec phase**, both of them renderer-boundary work rather
than UI work, and both enlarged by the `d3d11` default flip:

1. **Promote the composited overlay to a real path** — with a renderer-neutral home, because
   the CPU escape hatch would otherwise ship without a transport; and re-measure the cost
   against a fade **during a drag**, on `ui gap`, not against §19.3's static playback run.
2. **Add the view-transform contract to `VideoRenderer`** — one contract, both backends, and
   the D3D11 reduction taps derived from the post-transform fit.

**And two items get cheaper than the spec expects**: reverse shuttle (item 2) is a call
site, and zero-based video numbering (item 8) is already done.

Also: **`V:\` is live client production storage and is strictly read-only.** Any
shell-extension prototyping must be read-only against files Anj nominates.

---

## 3. The approved spec, as written

### Project context

Trace is a Windows 11 professional review player built with Qt Widgets and FFmpeg. A
separate GPU/smooth-presentation initiative is underway. The current CPU playback path is
the validated reference and must remain compiled, selectable and behaviorally unchanged.

This assignment covers interface integration and the explicitly defined user-facing features
below. Do not use this work as an opportunity to rewrite playback, decoding, rendering,
caching, scrubbing, timing, audio or remote-I/O systems.

### Non-negotiable architecture protection

Do not rewrite or destabilize: FFmpeg decoding; AVFrame or VideoFrame ownership; CPU/QImage
rendering; D3D11/DXGI rendering; renderer selection or fallback; presentation timing;
playback scheduler; audio clock or A/V sync; EOF drain; exact frame stepping; frame cache;
scrub landing; latest-target-wins behavior; color conversion or metadata handling;
LucidLink/remote I/O; buffering; performance telemetry.

The interface must call centralized controller/model actions. Do not create widget-local
playback timers, seek loops, reverse-decode loops or duplicate playback state.

If the controller lacks a required operation, first define the smallest renderer-neutral and
decoder-neutral command contract. Do not implement an alternate playback engine inside the UI.

### Audit first

Before editing, identify and report the boundaries of:

1. the main window and video-viewer classes;
2. all current transport controls;
3. the existing frame-forward and frame-back buttons;
4. keyboard shortcut handling;
5. the authoritative playback state and playback-rate owner;
6. exact-frame stepping APIs;
7. seek and scrub APIs;
8. fullscreen and window-state handling;
9. media metadata exposure;
10. renderer-neutral viewport-transform support;
11. current LucidLink/path detection, if any;
12. whether forward and reverse variable-rate playback already exist safely.

### Transport redesign

The current visible forward/back buttons perform single-frame stepping. Change their
user-facing purpose:

- Existing visible backward button becomes **Rewind / reverse shuttle**.
- Existing visible forward button becomes **Fast-forward / forward shuttle**.
- Replace their artwork with the approved continuous-scan icons.
- Remove visible one-frame Previous and Next buttons.
- Do not delete the underlying exact-frame-step commands.

Frame stepping becomes keyboard-only: Right Arrow advances exactly one frame, Left Arrow
moves exactly one frame backward. Frame stepping must retain current deterministic
behavior, and must not trigger while focus is inside a text-entry control.

### Zero-based frame numbering

- First displayed frame: 0. Last valid frame in an N-frame source: N−1.
- Go to Frame accepts 0 through N−1.
- Frame-count transport display shows the current frame index on the left.
- The right endpoint shows the last valid frame index, not the count.

Do not change internal frame identity merely to alter its displayed label. Confirm the
existing internal convention and perform presentation-layer conversion only if necessary.

### Fast-forward behavior

> **ENGINE STATUS 2026-08-10: the shuttle behind this is BUILT, MEASURED AND SIGNED OFF**
> (`dd21fe9`, plan §11a–§11b). Both directions run the 2×/5×/10×/30× ladder as sampling
> strides at a constant presentation cadence, achieved speed measured within a few percent
> of demanded on all four formats, and reverse 30× snaps to the keyframe grid at a stable
> ~15fps presentation. **The interface itself remains deferred and is not to be started.**
>
> **One thing this section must not lose when it is picked up.** The keyboard convention
> keeps 1× as its first rung — J and L step 1× → 2× → 5× → 10× → 30× — but **the BUTTONS
> must begin at 2× on their first click**, as this section already specifies. The owner
> confirmed both readings on 2026-08-10: the keyboard convention is not the button contract,
> and a button that inherits J/L's ladder wholesale would start at 1× and be wrong.
> `startShuttleRun(direction, stride)` takes any stride, so this is a call site rather than
> engine work.

Each normal activation advances one stage: **+2× → +5× → +10× → +30×**, and further presses
at +30× remain at +30× because that is the maximum.

- If paused, the first press begins forward playback at +2×.
- If playing normally, the first press changes to +2×.
- Pressing Fast-forward while rewinding changes direction and begins at +2×.
- Pressing Play while scanning returns to normal +1× playback.
- Pressing Pause pauses and clears the active shuttle rate.
- Opening new media starts at +1×.
- Show temporary indicators: 2×, 5×, 10×, 30×.
- The button must not depend on double-click timing. Every ordinary activation advances one stage.
- Repeated presses must not start overlapping timers or decoders.

### Rewind behavior

Mirrors Fast-forward exactly: **−2× → −5× → −10× → −30×**, further presses remain at −30×.

- If paused, the first press begins reverse playback at −2×.
- Pressing Rewind while scanning forward changes direction and begins at −2×.
- Pressing Play returns to normal +1× playback.
- Pressing Pause stops and clears the active shuttle rate.
- Show temporary indicators: −2×, −5×, −10×, −30×.

### Reverse-playback safety

Continuous reverse playback is not a cosmetic UI change. **Do not implement reverse playback
as repeated synchronous seeks from a UI timer.**

First determine whether the existing playback/cache architecture safely supports negative
playback rates while preserving: exact frame identity; responsiveness; cache ownership;
decoder ownership; stale-request dropping; audio state; renderer independence.

If it does not, implement only: the redesigned Rewind control; the controller/action
contract; capability detection; correct disabled or experimental state; tests for the UI
state — then document the missing playback-core work separately.

Do not destabilize the active GPU/scrub initiative to force reverse playback into this
interface commit.

### Audio during shuttle

Do not invent a new time-stretching system. Use existing playback-controller audio behavior
if non-1× rates are already supported. If no established policy exists, use this provisional
safe behavior:

- Shuttle playback at ±2×, ±5×, ±10× and ±30× is silent.
- Preserve the user's previous mute and volume settings.
- Restore the previous audio state when normal +1× playback resumes.
- Do not expose shuttle muting as if the user manually changed Mute.

Keep this policy isolated so it can be changed later.

### Playback speed menu

Explicit numeric labels only: **0.5× / Normal — 1× / 2× / 5× / 10× / 30×**. Do not use
ambiguous Fast/Faster labels. The checked item must reflect the effective playback rate.
Playback buttons, menus and shortcuts must share one source of truth. Negative rates remain
controlled through Rewind rather than duplicating every negative rate in the menu.

### Fullscreen consolidation

Remove QuickTime-inspired AirPlay and Picture-in-Picture positions from the transport. Use
one Fullscreen control:

- Enter Fullscreen when windowed; Exit Fullscreen when fullscreen.
- Same QAction/command as the View menu.
- Escape exits fullscreen. F11 and/or Alt+Enter toggle it.
- Double-clicking the video toggles fullscreen unless that conflicts with an established
  Trace gesture.
- Preserve and restore the prior window geometry.
- Enter fullscreen on the monitor containing the active window.
- Do not confuse fullscreen with maximize.
- Use native Windows titlebar controls and preserve Windows 11 Snap Layout behavior.

### Auto-hiding interface

Windowed and fullscreen: pointer movement over the video reveals the transport; clicking the
video reveals it; relevant keyboard input reveals it; inactivity fades it away. Fullscreen
also hides the cursor after inactivity. Provisional inactivity delay: **2 seconds**.

Do not hide controls while: the pointer is over the transport; timeline dragging is active;
volume dragging is active; a popup menu is open; a tooltip is open; a child control has
keyboard focus. Resume the hide timer after interaction ends.

**Overlay animation must not affect video cadence or copy video pixels.**

### Share / Copy menu

The transport Share button opens: **Copy File Path**, **Copy LucidLink Link**, **Show in
File Explorer**.

**Copy File Path** — copy the canonical Windows path to the clipboard; keep enabled for
ordinary file-backed media; disable for non-file sources; show non-blocking confirmation
"File path copied."

**Show in File Explorer** — open the containing folder and select the active media file; do
not start playback or transfer the file; handle a missing file cleanly.

**Copy LucidLink Link** — produce the *same* direct link that Windows Explorer's LucidLink
"Copy link" context-menu command produces. Classic example:

```
lucid://projects.omcprod/file/2955:105895/Universe_Full_Takes_v006.mp4?reveal=true
```

Newer LucidLink installations may produce an `app.lucidlink.com` HTTPS link instead. Accept
the actual format returned by the installed integration. **Never construct a `lucid://` or
HTTPS link by parsing or guessing from the mounted Windows path. Never hard-code filespace
IDs, inode/resource IDs, domains or URL formats.**

Implementation approach — read-only investigation of the installed LucidLink Windows
integration, in this preferred order:

1. Use an officially supported LucidLink API or command if one is available locally.
2. Otherwise use the registered Windows Explorer shell extension / context-menu command for
   the selected file.
3. Invoke the registered command through Windows shell interfaces rather than screen
   automation.
4. Do not simulate mouse clicks in File Explorer.
5. Do not depend on the command's menu position.
6. Do not hard-code "More options."
7. Prefer a stable canonical shell verb if the extension exposes one.
8. If only a display string is exposed, account for version/localization risk and document
   it before shipping.

Because the shell action may populate the clipboard asynchronously: preserve the previous
clipboard contents until invocation succeeds; observe clipboard changes with a short
non-blocking timeout; validate that the result is a supported LucidLink direct-link form;
never freeze the UI while waiting; show "LucidLink link copied" on success; on failure
explain that the integration is unavailable; **never overwrite the clipboard with an invalid
or guessed value.**

Enable Copy LucidLink Link only when the source is file-backed, the path is on a
LucidLink-mounted filespace or the shell extension declares support, and the integration is
available. **Do not assume that all `V:\` paths are LucidLink.** A copied link does not grant
permissions — do not display language implying it makes a file public.

### Temporary rotate and flip

Rotate Left, Rotate Right, Flip Horizontal, Flip Vertical, Reset View Transform.

Temporary viewing transforms only: never modify source media; never write metadata to the
file; never remux or export. Reset the transform when new media opens. Maintain the same
transform while seeking, scrubbing and playing the current media. Do not change authoritative
frame identity. Apply through a renderer-neutral view-transform contract, and keep CPU and
GPU behavior visually equivalent. If no safe renderer-neutral contract exists, implement the
actions/capability boundary and defer the rendering changes rather than separately hacking
the CPU and GPU paths.

### Time display

Elapsed Time, SMPTE Timecode, Frame Count, Go to Timecode…, Go to Frame…

SMPTE: enable only when valid timecode exists in the source; use embedded source timecode;
do not generate SMPTE from zero when none exists; do not label an elapsed-time conversion as
source timecode; respect the source rate and drop-frame/non-drop-frame metadata; disable Go
to Timecode when valid source timecode is unavailable.

Frame Count: zero-based; derived from the authoritative displayed-frame identity; never
derived from rounded wall-clock time.

Go to Frame: accept integers from 0 through the final valid frame index; validate before
seeking; use the existing exact-frame seek path.

### Menus

**File** — Open File…, Open Recent, Close Media, Exit.

**Edit** — Copy Current Frame (only if safely supported), Rotate Left, Rotate Right, Flip
Horizontal, Flip Vertical, Reset View Transform.

**View** — Enter/Exit Fullscreen, Always on Top, Actual Size, Fit to Window, Zoom In, Zoom
Out, Playback Speed, Time Display, Loop.

**Window** — Minimize, Maximize/Restore, Actual Size, Fit to Window, Show/Hide Movie Inspector.

**Help** — Trace Help, Keyboard Shortcuts, Report an Issue, Check for Updates (only if an
updater exists), About Trace.

### Features to ignore in this pass

Audio-track selection; language selection; subtitle selection; chapter navigation; Show
Clips; Show Audio Track; Tabs; Spatial Video; AirPlay; Picture-in-Picture; macOS tiling
commands; Bring All to Front; Export Progress; destructive media editing.

### Open Recent

Maintain a bounded recent-file list with Clear Recent Files. Do not probe every path during
application startup. Do not block on disconnected LucidLink/network paths. If a selected
recent file is missing, report it and offer to remove the entry. Store canonical paths. Do
not log sensitive path history unnecessarily.

### Movie Inspector

Non-blocking, with collapsible sections.

**General** — filename, source path, resolution, file size, overall data rate, current
viewport size, container, video format, audio format.

**Video details** — exact encoded FPS rational plus readable decimal, bitrate, pixel aspect
ratio, display aspect ratio, current scale, pixel format, bit depth, color primaries,
transfer characteristics, matrix coefficients, full/limited range, codec/profile, track ID.

**Audio details** — codec, sample rate, bitrate, channel layout, track ID.

Rules: display Unknown or Untagged honestly; do not infer missing color metadata inside the
inspector; distinguish encoded metadata from playback inference; long paths must be copyable;
update when active media changes; do not continuously poll expensive decoder state; do not
block remote-media opening to calculate optional values.

### Shared actions and state

Use one QAction/command source wherever possible for: Play/Pause, Rewind, Fast-forward,
Fullscreen, Always on Top, Loop, Actual Size, Fit to Window, Zoom, Rotate/Flip/Reset, Time
Display, Inspector visibility. Menus, buttons, tooltips, shortcuts, checked states and
accessibility names must remain synchronized.

### Keyboard

Space = Play/Pause. Right Arrow = exactly one frame forward. Left Arrow = exactly one frame
backward. Escape = exit fullscreen. F11 and/or Alt+Enter = fullscreen. Ctrl+O = Open.
Ctrl+I = Movie Inspector. Ctrl+0 = Actual Size. Ctrl++ / Ctrl+− = Zoom. M = Mute if
unclaimed.

Preserve existing shortcuts when they conflict unless product review explicitly approves the
change.

### Performance requirements

No new video-frame copies. No filesystem probing in paint or timeline updates. No blocking
shell-extension calls on the UI thread. No per-frame UI object allocation. No overlay
animation tied to frame decoding. No inspector polling in the presentation path. No UI-thread
reverse-seek loop. No startup delay caused by recent LucidLink paths. **No changes to
renderer timing or presentation cadence.**

### Implementation phasing

Small, independently reviewable commits:

1. UI/controller audit
2. Shared actions and transport artwork integration
3. Keyboard-only frame stepping and shuttle control contracts
4. Forward shuttle behavior
5. Reverse shuttle behavior — only if safely supported
6. Fullscreen consolidation and overlay auto-hide
7. Time Display and zero-based frame UI
8. Share menu and ordinary path copying
9. LucidLink shell-integration prototype and validation
10. Temporary view transforms
11. Open Recent
12. Movie Inspector
13. Menus, help and accessibility polish
14. Full regression pass

Do not combine uncertain LucidLink shell work or reverse-playback architecture with
otherwise safe visual changes in one commit.

### Validation

First frame displays as frame 0; Right Arrow advances one exact frame; Left Arrow reverses
one exact frame; no on-screen frame-step buttons remain; forward sequence 2×→5×→10×→30×;
reverse sequence −2×→−5×→−10×→−30×; further same-direction presses remain at 30×;
opposite-direction press starts at 2× in the new direction; Play restores 1×; Pause clears
shuttle state; rate indicators correct; local path copying; LucidLink classic `lucid://` link
copying; new LucidLink HTTPS link copying if applicable; LucidLink integration missing;
non-LucidLink `V:\` path; file removed while open; Show in Explorer; fullscreen on primary and
secondary monitors; overlay auto-hide during playback and pause; overlay stays visible while
interacting; zero-based Go to Frame validation; SMPTE source present; SMPTE source absent;
rotate/flip with CPU renderer; rotate/flip with GPU renderer when available; new media resets
transforms; recent LucidLink path unavailable; high-DPI scaling; keyboard-only navigation;
CPU reference regression; exact scrub and frame-step regression; audio and A/V-sync
regression.

### Final report

Return: (1) architecture audit; (2) files/classes changed; (3) commit hashes by phase;
(4) implemented features; (5) disabled/deferred capabilities; (6) reverse-playback findings;
(7) exact LucidLink integration method; (8) supported LucidLink link formats observed;
(9) confirmation that links are obtained from LucidLink rather than reconstructed;
(10) CPU/GPU visual-transform comparison; (11) playback and scrub regression results;
(12) confirmation that playback, renderer, cache, timing, audio and I/O architecture were
preserved; (13) remaining risks; (14) clean git status.

Stop after this interface pass. Do not begin unrelated GPU, decoder, export, subtitle,
chapter or media-editing work.

---

## 4. Media-driven window size (part of the same deferred pass)

The normal Trace window must adopt the active media's exact display aspect ratio so the video
viewport contains no black bars.

### Calculate the correct ratio

Use the final display aspect ratio, accounting for: encoded width and height; sample/pixel
aspect ratio; display-aspect-ratio metadata when authoritative; rotation/orientation
metadata; temporary Rotate Left/Right viewing transforms. **Do not assume display ratio is
always encoded width divided by encoded height.**

### Opening media

When media opens in a normal unsnapped window:

1. Determine the final display aspect ratio.
2. Start with the source's natural displayed size when practical.
3. If that does not fit the current monitor's available work area, scale it down proportionally.
4. Reserve a safe margin around the outer window.
5. Account for the Windows titlebar, menu and window frame.
6. Size the **video client area** — not merely the outer frame — to the exact media ratio.
7. Center the resized window within the current monitor's available work area.
8. Do not move the window unexpectedly to a different monitor.
9. Do not upscale small media beyond its natural displayed size by default unless existing
   product behavior specifies otherwise.

Use DPI-aware window calculations. Recalculate correctly when the window moves between
monitors with different scaling.

### No-bar normal window

In normal unsnapped mode: the video viewport matches the media aspect ratio; the image
touches all four viewport edges; do not letterbox, pillarbox, crop or stretch; overlay
controls do not participate in layout sizing; the transport floats over the video.

### Aspect-locked resizing

Add a checked-by-default setting: **View > Lock Window to Media Aspect Ratio**.

While enabled in normal mode: interactive resizing preserves the media's display aspect
ratio; the dragged edge or corner remains authoritative; adjust the other dimension smoothly;
avoid resize-event recursion and visible oscillation; respect minimum usable player
dimensions; keep window chrome and DPI scaling in the calculation; re-evaluate the ratio
after a temporary 90-degree rotation; restore the original media ratio when transforms reset.

When disabled: the user may freely resize; fit the complete image inside the viewport; black
bars may appear because the user explicitly unlocked the ratio.

### Window-state exceptions

An exact no-black-bar guarantee cannot apply when Windows controls the window dimensions and
those dimensions do not match the media. For fullscreen, maximized windows, Windows Snap
Layouts, and externally imposed geometry: preserve the complete image; preserve the correct
aspect ratio; use neutral black letterboxing or pillarboxing where unavoidable; never
automatically crop; never stretch; **do not fight Windows by continuously resizing a snapped
or maximized window.**

Returning from fullscreen, maximize or snap to normal mode: restore the previous normal
window position when valid; reapply the media aspect lock; ensure the restored window remains
inside the active monitor's work area.

### Opening another file

If the window is normal and not actively being resized, update it to the new media ratio;
keep it on the same monitor; scale down if necessary; do not exceed the available work area.
If maximized, snapped or fullscreen, keep the current window state and fit the new image
until normal mode resumes.

### Validation

16:9; 9:16; 4:3; 1:1; 2.39:1; anamorphic non-square-pixel media; rotation metadata; temporary
90-degree rotation; very small source; 4K/8K source larger than the desktop; 100%, 125%, 150%,
200% and mixed-monitor DPI; primary and secondary monitors; taskbar on different edges; aspect
lock enabled and disabled; normal, maximized, snapped and fullscreen states; opening
differently shaped files consecutively; CPU and D3D11 renderer paths.

Confirm this work changes **window/view geometry only** and does not change decoded pixels,
frame identity, scaling-quality selection, renderer ownership or presentation timing.
