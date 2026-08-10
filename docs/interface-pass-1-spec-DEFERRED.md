# Interface pass 1 — approved spec, DEFERRED

**Status: NOT STARTED. Do not begin any of this.** Interface work is paused by owner
priority (see `CLAUDE.md`). This document exists so the spec and the assets are recorded
now and are ready when the GPU initiative completes.

**Unblocks when:** the GPU/smooth-presentation initiative reaches a state the owner signs
off — realistically after GATE E (plan §8 item 11), since several items below depend on
decisions that are still open inside it. See "Dependencies and conflicts" before planning
any of it.

Author: Anj, 2026-08-09. The spec body in §3 is his and is reproduced as approved.

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

## 2. Dependencies and conflicts to resolve BEFORE scheduling this work

These are flagged now because each one is cheaper to answer during the GPU work than after.
None of them is a reason to change the spec yet; they are the reasons this pass cannot
simply be picked up as written.

1. **The auto-hiding transport floating over the video is blocked on the overlay
   question, and that question is currently answered "no".** Plan §19 / §20.1: ordinary Qt
   child widgets over the D3D11 child HWND are **neither visible nor hit-testable**, and
   every native-window variant loses translucency. Renderer-composited translucency was
   proven to work with no measured playback cost, but `TRACE_OVERLAY_COMPOSITED` is
   explicitly a **disposable spike with placeholder art**, off by default. Today the
   transport is a laid-out widget *below* the viewer, not over it.

   So "the transport floats over the video" and "overlay controls do not participate in
   layout sizing" are not styling changes — they require the composited-overlay path to be
   built for real, on the GPU backend, before any of the auto-hide behaviour can exist.
   **This is the single largest structural item in the whole spec.** It should be a
   deliberate deliverable of the GPU phase, not a surprise discovered in interface phase 6.

2. **Reverse shuttle (−2× … −30×) is the known-hard case and will likely defer on first
   pass.** Roadmap item 6: continuous reverse playback beyond the reverse cache is still
   GOP-walk bound on long-GOP H.264. Dragging is fast because it walks sequentially and the
   cache absorbs misses; reverse *playback* does not share that. The spec already handles
   this correctly (capability detection, disabled/experimental state, document the missing
   core work separately) — expect that branch to be the one taken for H.264, and note that
   ProRes may support it immediately since every frame is a keyframe.

3. **Shuttle rates must share one owner with J-K-L, which already exists.**
   `PlaybackController` is the authoritative mode/speed state machine and J-K-L already
   drives off-speed jog. The 2/5/10/30 button ladder must be a second *view* onto that
   state, not a second rate machine. Note J-K-L above 1× currently clears `userPlayIntent_`
   (`473b90e`); the ladder needs the same treatment or Play/Pause across a drag breaks.

4. **The audio-during-shuttle policy the spec proposes is already the shipped policy.**
   Audio is 1× forward only; off-speed, reverse, scrub and step are deliberately silent,
   with one guard in the tick catching every way playback stops being 1× forward. So
   "provisional safe behavior" is simply "keep what exists" — no new work, and no new
   policy to isolate.

5. **LucidLink detection already exists and is reusable — do not write a second one.**
   `MediaIoSource` classifies storage per volume by *querying* it (LucidLink presents as
   `DRIVE_FIXED`/NTFS and is recognised by advertising petabyte capacity with
   `free == total`), never by drive letter or volume label, and never by writing a probe
   file. That is exactly the spec's "do not assume all `V:\` paths are LucidLink"
   requirement, already solved and already cached per volume.

   Also: **`V:\` is live client production storage and is strictly read-only.** Any
   shell-extension prototyping must be read-only against files Anj nominates.

6. **Rotate/flip needs a transform concept the renderer boundary does not have yet.**
   `VideoRenderer` (`src/render/VideoRenderer.h`) has no view-transform contract. The
   D3D11 backend already computes a letterbox **viewport**, which is the natural place for
   an orientation transform to live; the CPU backend would apply it in its `QPainter`. The
   spec's own fallback (define the action/capability boundary, defer the rendering change)
   is the right call if the contract isn't ready. Best outcome: add the transform to the
   `VideoRenderer` interface *during* the GPU phase, while both backends are being touched
   anyway, so the interface pass only has to wire actions to it.

7. **Aspect-locked resizing will interact with the frame cache.** `syncScrubPreviewSize()`
   calls `reclaimDecoder()` and **clears the decoder's frame cache** on every resize,
   because cache entries carry the preview size in force when they were made. Continuous
   aspect-locked drag-resizing would therefore thrash the cache repeatedly. Under owner
   priority #1 this needs a resize-settled debounce, or a cache that tolerates mixed sizes.
   Measure it; do not assume it is free.

8. **Zero-based numbering is probably already the internal convention.** The HUD and
   `PlaybackController` work in frame indices from 0. Confirm before writing any conversion
   layer — the spec is explicit that internal frame identity must not change to alter a
   displayed label, and the likely correct outcome is that no conversion is needed at all.

9. **Exact source timecode is not currently read.** The Movie Inspector's "exact encoded FPS
   rational" is now available (`7b924be`, stored as `int`/`int` on `VideoMetadata`), but
   SMPTE start timecode and drop-frame metadata are not extracted today. The spec's rule —
   never generate SMPTE from zero, never label an elapsed-time conversion as source
   timecode — means this needs real extraction work, not a formatter.

10. **Double-click on the video to toggle fullscreen** — confirm it does not collide with
    any established Trace gesture before claiming it.

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
