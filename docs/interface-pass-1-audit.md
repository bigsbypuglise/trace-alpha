# Interface pass 1 — phase 1 audit (2026-08-10)

The spec's phase 1 is "identify and report the boundaries **before editing**". This is that
report. It is read-only: nothing here was changed to make it true, and where the answer is
uncomfortable it is written down rather than fixed in passing.

Its twelve numbered sections are the spec's twelve, in the spec's order. Each ends with
**what it means for the pass**, because a boundary nobody acts on is a paragraph, not an
audit.

Two prerequisites landed before this audit and are therefore part of its subject matter:
the renderer-neutral overlay (`5e1f834`) and the view-transform contract (`4b7174f`).

---

## 1. Main window and video-viewer classes

`trace::app::MainWindow` (`src/app/MainWindow.cpp`, 3.5k lines) owns everything: the media,
the decoder, the audio output, the playback timer, the scrub worker lease, every QAction and
the whole HUD. It is not a controller with a view; it is the application.

`trace::ui::ViewerWidget` (`src/ui/ViewerWidget.*`) hosts a `VideoRenderer` and owns the
scheduling around it — when a repaint was asked for, how long it took to arrive, which frame
is current. Since `5e1f834` it also owns the `OverlayModel` and the `ViewTransform`, both
deliberately: they outlive any one backend, and a renderer that fails to initialize is
replaced by the CPU one, which then inherits both rather than silently resetting them.

Layout is a plain `QVBoxLayout` on the central widget (`MainWindow.cpp:720-734`):

```
viewer_          (stretch 1)
transportBar_    (fixed)
overlay_         (the HUD, fixed height, grows with the media)
```

**Nothing floats over anything.** The HUD is *below* the video, not on it.

> **For the pass:** §4's "the transport floats over the video" and "overlay controls do not
> participate in layout sizing" are a change to this layout, not styling. The mechanism now
> exists on both backends; what has not happened is removing `transportBar_` from the layout,
> which is phase 6 and must not happen before the overlay carries the same controls.

## 2. Current transport controls

| control | where | action |
|---|---|---|
| Previous Frame | `TransportBar` button | `prevFrameAction_` — one exact frame back |
| Play / Pause | `TransportBar` button | `playPauseAction_` |
| Next Frame | `TransportBar` button | `nextFrameAction_` — one exact frame forward |
| Fullscreen | `TransportBar` button | inline lambda, **not** a QAction |
| timeline | `QSlider` inside `TransportBar` | press / valueChanged / release, wired in `MainWindow` |
| frame readout | `QLabel` inside `TransportBar` | `setFrameText("%1 / %2")` |
| File > Open | menu | `openAction` (Ctrl+O) |
| File > Toggle Fullscreen | menu | inline lambda, Ctrl+Return |
| File > Quit | menu | `quitAction` |

`TransportBar` emits intent only — `prevFrameClicked`, `playPauseClicked`, `nextFrameClicked`,
`fullscreenClicked` — and holds no playback state. That part of the spec's "shared actions"
requirement is already satisfied for three of the four.

**Fullscreen is the exception and it is duplicated.** The same four lines
(`setWindowState(... ^ Qt::WindowFullScreen)`, `viewState_.fullscreen`,
`transportBar_->setFullscreen()`, `refreshHud`) appear twice, at `MainWindow.cpp:805-808` and
`:885-888`, once for the menu and once for the button. There is no `fullscreenAction_` member.

> **For the pass:** phase 2 has one real job here and it is small — promote fullscreen to a
> QAction like the other three, so the menu, the button and the future keyboard shortcut are
> one source. Everything else in "shared actions and state" already holds.

## 3. The frame-forward and frame-back buttons

`prevFrameAction_` / `nextFrameAction_`, `MainWindow.cpp:821-873`. Each does the same five
things: step the controller, pause, stop the timer and audio, **clear `userPlayIntent_`**, and
issue a `RequestMode::Step` request with a rollback if the load fails.

They are also what the **composited overlay's Rewind and Fast-forward currently call**
(`installOverlayHooks`, `:757-791`). The overlay's artwork is already continuous-scan
chevrons, so the picture and the behaviour disagree there today — knowingly, and recorded in
`OverlayHooks.h` rather than papered over with a rename.

> **For the pass:** the spec removes these two *buttons* and keeps the *commands*. Do not
> delete the actions: keyboard stepping, the overlay hooks and the failure-rollback all go
> through them. Phase 3 re-points the visible controls; the actions stay exactly as they are.

## 4. Keyboard shortcut handling

One `MainWindow::keyPressEvent` (`:3366-3512`), a flat switch, no shortcut table:

`Space` play/pause · `M` mute · `Left`/`Right` exact single-frame step · `J`/`K`/`L` shuttle ·
`F`/`S`/`T` readout mode (frame / seconds / timecode) · `I` info · `Return` HUD visibility.
Menu accelerators supply Ctrl+O and Ctrl+Return.

Two deliberate protections exist and are load-bearing:

- `timelineSlider_->setFocusPolicy(Qt::NoFocus)` (`:1006`), so arrows never reach the slider;
- an event filter on the slider (`:990-995`) classifying a **wheel notch over the groove** as
  a stepping gesture, because it is the one route into `valueChanged` that is not part of a
  drag and would otherwise leave `userPlayIntent_` set with no release to restore it.

**There is no text-entry control anywhere in the application**, so the spec's "frame stepping
must not trigger while focus is inside a text-entry control" has nothing to guard today. Go to
Frame and Go to Timecode (phase 7) create the first one.

> **For the pass:** the spec's keyboard map collides with the existing one in one place —
> `F11`/`Alt+Enter` for fullscreen against the current `Ctrl+Return`, and `Escape` which is
> currently unhandled. `F`, `S` and `T` are Trace's own and are not in the spec's list; the
> spec says preserve existing shortcuts on conflict, and there is no conflict, so they stay.
> A shortcut *table* is worth building at phase 3 rather than extending the switch, because
> phase 13 has to render a Keyboard Shortcuts window from something.

## 5. The authoritative playback state and rate owner

`trace::core::PlaybackController` (`src/core/PlaybackController.*`, 106 lines) owns mode,
speed, current frame and max frame, and nothing else. Both shuttle ladders live in it as
1/2/5/10/30, forward and reverse. `MainWindow` owns the *run mechanics* — `shuttleDir_`,
`shuttleStride_`, `shuttleSnapping_` — all derived from `playback_.state().speed`.

Three findings, all of which shape phases 3-5:

**(a) The button contract and the keyboard contract genuinely differ, and only the keyboard
one is expressible today.** `jogForward()` and `jogReverse()` enter the ladder at **1×** from
a stop, and both **reset to 1× on a direction change**. The buttons must start at **2×** in
both of those cases (owner-confirmed, 2026-08-10). This is not a call site poking `speed`
from outside; it is a second documented way into one rate machine, and it belongs in the
controller.

**(b) `startShuttleRun` has exactly two callers and each performs a fixed five-step
sequence**: `endShuttleRun` → controller ladder → `prepareVideoRequest` →
`beginPlaybackTimeline` → `startShuttleRun`. §29.2 is the standing warning — GATE E was
validated on the Play action alone and every other path that started the timer compiled
silently and decayed quadratically. **Extract the sequence before adding a third caller.**

**(c) `userPlayIntent_` is set differently by each entry point** and the difference is
deliberate: `J` clears it (`:3416`); `L` sets it to `|speed| <= 1.0001` (`:3460`), because 1×
forward is worth restoring after a drag and an off-speed run is not. The button ladder needs
the same treatment or Play/Pause across a drag breaks.

## 6. Exact-frame stepping APIs

`PlaybackController::stepForward()` / `stepBackward()` move the index and clamp;
`VideoDecoderFFmpeg::RequestMode::Step` is what makes the *decode* exact. Step is the only
mode that gets `SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND`, and `tryReverseCache` refuses
`previewRes`-tagged entries for anything but a Scrub — so a step can never be served a frame
converted at preview resolution.

Both keyboard step keys call `endShuttleRun(landExactly=false)` first, because the step lands
its own frame and paying for two landings would put a stale one in front of it.

> **For the pass:** untouched by every phase. The spec is explicit that the commands survive
> the buttons being removed.

## 7. Seek and scrub APIs

There is no `seek(frame)` on `MainWindow`. Scrubbing is expressed as **press / move / release**
on the QSlider, and everything else routes through it:

- `sliderPressed` — ends any shuttle run, sets `scrubbing_`, sets `scrubJumpPending_` so the
  first flush lands exactly, suspends playback **without touching `userPlayIntent_`**;
- `valueChanged` — `queueVideoScrubFrame()`, coalesced by a 12ms timer, walked by the async
  worker under the decoder lease;
- `sliderReleased` — `flushVideoScrub(true)` for the exact landing, then
  `resumePlaybackAfterScrub()`, **in that order** (the resume needs the lease back, and
  starting audio earlier would start it at the preview position).

`OverlayHooks::setScrubbing` / `seekToFraction` drive that same slider rather than seeking, so
the overlay inherits the drag shuttle, the exact landing and the step-5.6 play-state restore
without reimplementing any of it. Verified in the drag measurement: the overlay-driven drag
reports `scrub exact | target 1 | shown 1 | delta 0`.

> **For the pass:** Go to Frame and Go to Timecode (phase 7) must go through the slider or
> through `RequestMode::Step` — not through a new seek entry point. `setValue()` on the
> slider with no press is already a known-classified gesture (the wheel filter), which is the
> pattern to copy.

## 8. Fullscreen and window-state handling

`setWindowState(windowState() ^ Qt::WindowFullScreen)`, duplicated as noted in §2.
`viewState_.fullscreen` mirrors it and `transportBar_->setFullscreen()` swaps the icon.

**Everything else the spec asks for is absent.** No geometry save/restore, no monitor choice,
no Escape handling, no distinction from maximize, no `WM_DPICHANGED` handling. `ViewerWidget`
implements **no mouse handlers at all** on the CPU path except the overlay ones added at
`5e1f834`, and the only `MouseButtonDblClick` in the tree is in `src/ui/OverlaySpike.cpp:31`,
the superseded Qt-widget probe — so **double-click to toggle fullscreen collides with
nothing**.

One thing to carry into phase 6: `MainWindow::resizeEvent` calls `syncScrubPreviewSize()`,
which calls `reclaimDecoder()` **unconditionally and before** the size comparison, and then
`setScrubPreviewSize()`, which is guarded and clears the frame cache only on a real size
change (`VideoDecoderFFmpeg.cpp:599`). A fullscreen transition is one size change, so it is
one cache clear; **aspect-locked continuous drag-resizing (spec §4) is a size change per
event** and would clear the cache on each. Two independent fixes: hoist the comparison above
`reclaimDecoder()`, and debounce resize-settled. Measure both.

## 9. Media metadata exposure

`VideoMetadata` (`src/core/VideoDecoderFFmpeg.h:10-33`) carries width, height, `fps` as a
double, **`fpsNum`/`fpsDen` as the exact rational**, `frameCount`, `durationSeconds`,
`codecName` and `intraOnly`. Colour metadata (matrix, range, whether inferred) reaches the HUD
through `VideoPerfStats`, not through `VideoMetadata`.

`metadata()` is the one thing the HUD reads live while a lease is out, and that is safe by
construction: only `open()` writes it, and `open()` cannot run while a lease is out.

**Not present, and the Movie Inspector wants them:** bitrate, pixel aspect ratio, display
aspect ratio, pixel format as a string, bit depth, colour primaries, transfer characteristics,
track IDs, audio codec/sample rate/bitrate/channel layout, file size, container name.

**And SMPTE timecode is not merely absent — the current readout is already the thing the spec
forbids.** `TimeFormat::frameToTimecode(frame, fps)` synthesises a timecode from the frame
index, and the HUD prints it under the label `Timecode:` (`MainWindow.cpp:2762`, and the `T`
key selects it). The spec says "do not label an elapsed-time conversion as source timecode".

> **For the pass:** phase 12 needs real extraction work on `VideoMetadata`, and phase 7 has to
> decide what the existing readout becomes when a source carries no timecode: relabel it as
> elapsed, or disable it. Doing nothing leaves a non-conforming label in place.

## 10. Renderer-neutral viewport-transform support

**Exists as of `4b7174f`.** `VideoRenderer::setViewTransform(const ViewTransform&)`, honoured
by both backends: D3D11 in the vertex shader's texture coordinate, CPU in a `QPainter`
transform. Both fit the *displayed* size, so a quarter turn re-letterboxes; D3D11 recomputes
step 9's reduction taps from the post-transform fit and exchanges the footprint axes.

`ViewerWidget` holds the transform and re-applies it to whatever backend is adopted.
`TRACE_VIEW_TRANSFORM` drives it for now; the HUD reports `view rot90 flipH`.

> **For the pass:** phase 10 is now wiring only — five QActions onto
> `viewer_->setViewTransform()`, plus reset on new media. The spec's fallback ("define the
> actions, defer the rendering") is no longer needed.

## 11. LucidLink / path detection

`MediaIoSource` (`src/core/MediaIoSource.cpp:244-257`) classifies storage **per volume, by
querying it**: `DRIVE_REMOTE` is definitive, and a virtual mount is recognised by advertising
petabyte-scale capacity with `free == total`. Never by drive letter, never by volume label,
never by writing a probe file. Cached per volume.

**But it answers a storage-class question, not a vendor one.** "Virtual mount with
`free == total`" is true of any such mount, not only LucidLink.

> **For the pass:** reuse it, do not write a second one — and do not let it be the enabling
> condition for *Copy LucidLink Link* on its own. It is a good necessary condition and a bad
> sufficient one; the authoritative gate is the installed shell integration. Using the
> classifier alone reintroduces "assume all `V:\` paths are LucidLink" one level up.
> `V:\` is live client production storage and every probe must be read-only.

## 12. Forward and reverse variable-rate playback — does it already exist safely?

**Yes, in both directions, and it is signed off.** `e9fd236` and `dd21fe9`; owner retest
2026-08-10. Reverse decode runs on the existing scrub worker under the existing lease, results
are queued, the tick pops one per slot, and **the stride is the commanded speed** while
presentation stays at one frame per source period.

Measured achieved speed, forward, all sixteen cells: 1080p 2.04/5.02/10.3/28.1× · 4K H.264
2.06/4.86/12.0× · 422 HQ 2.06/5.27/11.3× · 4444 1.89/5.17/10.8×. Reverse 1× on 4K H.264 went
87.0 → 99.2% of real time. Reverse 30× snaps to the keyframe grid at a stable ~15 presents/s.

Against the spec's own safety list: exact frame identity (`delta 0` on every landing);
responsiveness (immediate, newest-target-wins by generation bump); cache ownership and
decoder ownership (one lease, returned through `reclaimDecoder()` alone); stale-request
dropping (the generation counter); audio state (silent above 1× through one guard,
`audioShouldDrive()`); renderer independence (nothing in the shuttle touches a backend).

> **For the pass:** §2 item 2 of the spec is stale. The capability-detection-and-defer branch
> is not the expected outcome; phase 5 is a call site onto `startShuttleRun(direction,
> stride)`, which takes any stride. Keep the capability check as a guard, not as a plan.

---

## What the audit changes about the plan

**Cheaper than the spec expects:** reverse shuttle (§12) is a call site; the view transform
(§10) is wiring; zero-based video numbering is already correct *including the right endpoint*
(`syncTransportBar` prints `currentFrame / maxFrame`); the audio-during-shuttle policy is
already the shipped policy and already isolated in one predicate.

**More than the spec expects:** the image-sequence and still HUD lines print
`currentFrame + 1` (`:3344`, `:3355`) and are the only one-based displays in the app; the
`Timecode:` label is already non-conforming; fullscreen is duplicated rather than shared; and
there is no shortcut table for phase 13 to render.

**The two things most likely to cost performance, and they are early**, exactly as the
session brief says: removing `transportBar_` from the layout in favour of the floating overlay
(phase 6), and aspect-locked resizing against the cache clear (spec §4). Both must be measured
when they land, not at phase 14.
