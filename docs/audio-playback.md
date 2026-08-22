# Audio file playback (2026-08-21)

Trace opens audio-only files — `wav mp3 m4a aac flac ogg opus` — and the
transport drives them. Deliberately small: no waveform, no visualiser, no new
subsystem. The empty-state prism mark stays on screen (there is no picture),
the top chrome carries the filename, and the strip's slider, readouts, Go To,
Home/End, stepping, Loop and the accessibility proxies all work unchanged.

## The shape

**`MediaKind::AudioFile` is the fourth branch of `openPath`'s extension
dispatch**, beside VideoFile / ImageSequence / StillImage. `AudioOutput`
already owned its own demuxer, decode thread, seek-to-offset and master clock
— built independent of the video decoder on purpose — so opening it *is* the
open. `frameSource_` stays null, and that null is the design: every
frames-available consumer asks the kind (or finds its existing guard already
refusing) rather than assuming media-open implies frames-open.

**The synthetic frame index: `duration × 24.0` (`kAudioNominalFps`).** The
transport speaks in frame indices, so an audio file gets a frame count derived
from its container duration at a nominal 24 fps. 24 rather than anything
cleverer because it is the fallback `readoutTextAt` and `refreshHud` already
used for a null `frameSource_`, so the three cannot disagree.

**The default readout for audio is Elapsed, never Frame Count.** A synthesised
frame number presented as the primary readout is the one way this feature
reads dishonestly in a tool whose pillar is trustworthiness. It is a default,
not a lock — the menu still offers Frame Count, the HUD prints the count as
`F:n/m @ 24fps nominal`, and the Movie Inspector lists the index under the
`playback` origin tag with "(synthesised by Trace — the file has no frames)".
`hasSourceTimecode_` is false, so SMPTE correctly declines.

**Playback is the audio-mastered path with everything frame-shaped removed.**
The tick lambda takes an early branch into `runAudioOnlyTick()`: the device
clock picks the playhead position (wall-clock accumulator while the device
primes, exactly per `clockReady()`'s contract), nothing decodes, the viewer is
repainted once per tick so the strip's thumb and readout move, and reaching
the end stops — or wraps, because `loopWrap` gained an audio branch (a wrap is
a device seek alone; there is no landing decode to pay).

**Dragging the slider is silent; the release seeks and resumes.** The
non-video slider branch moves the playhead and the readout only; the release
goes through `resumePlaybackAfterScrub()` → `startPlaybackRun()` →
`AudioOutput::start(offset)`, which already does exactly this. Scrub audio is
explicitly out of scope, per the standing rule (audio is 1x forward only).

**The mark draws for free.** `setMediaPresent` stays the renderers' own
two-state answer — no frame is ever set, so both backends report no picture
and draw the empty state. The one addition is
`OverlayModel::setPicturelessMediaOpen`: with an audio file open the "Drop
media or File › Open" hint would be an instruction to open a file that is
already open, so the hint line alone is suppressed and the mark stands
centred by itself. Close Media restores it.

**§4 leaves the window alone.** An audio file has no aspect ratio;
`currentDisplayAspect()` reads 0 and the shaping pass declines. One real
hazard was closed here: `releaseCurrentMedia` never cleared `currentImage_`
(the frames-backed kinds always overwrite it), so an audio open after a still
would have inherited the previous picture's size into the shaping pass. The
audio branch resets it.

## The guard audit

Every frames-available assumption was audited before anything changed.

Safe by construction — the existing guard already refuses (no change made):

| consumer | guard |
|---|---|
| Scrub worker, lease, drag chain, paint gate | `isVideoScrubActive()` = kind is VideoFile; worker never started for audio |
| Shuttle engine (`startShuttleRun`) | kind-is-VideoFile test |
| Playback prefetch (`startPlaybackPrefetch`) | kind-is-VideoFile test |
| J / L keys, `setPlaybackSpeed` | `!frameSource_` refusal (J/L are inert on audio; K still stops) |
| `prepareVideoRequest` | `videoFrameSource()` null → no-op |
| `requestExactFrameAsync` | `isVideoScrubActive()` refusal |
| §4 window shaping | `currentDisplayAspect()` reads 0 → declines (plus the `currentImage_` reset above) |
| Source timecode / SMPTE readout | `refreshSourceTimecode` finds no source → declines, resets a stale Timecode mode |
| Share menu | path-based; works for audio |
| `captureDecoderTelemetry` in refreshHud | video-gated already |
| Renderers' `setMediaPresent` | asked of the picture; no frame set → mark draws |

Changed — audio either allowed through or explicitly disabled:

| consumer | disposition |
|---|---|
| Play tick | early `runAudioOnlyTick()` branch |
| `togglePlayPause`, `resumePlaybackAfterScrub` | playable-range asked of playback state for audio |
| `audioShouldDrive` / `startAudioForPlayback` | AudioFile is an eligible kind; offset at the nominal rate |
| `stepOneFrame` | playhead move alone; no decode, no landing |
| `goToFrame`, both prompts, Go to End action | maxFrame from `playback_.state()` (identical for frames-backed kinds) |
| Slider valueChanged / sliderReleased (non-video branch) | audio branch: move + repaint; release resumes |
| `loopWrap` | audio branch: device seek, timeline re-established |
| Copy Current Frame | **disabled** (no frame) |
| Edit menu view transforms | **disabled** (no picture) |
| Zoom ladder / Actual Size / Fit (and therefore pan) | **disabled** (pan cannot engage below a zoom) |
| Playback speed rungs | **disabled** (audio is 1x forward only; a rate menu whose every rung silences the file is a menu of ways to mute it) |
| Rewind / Fast-forward strip buttons | **disabled** (the shuttle is a decode pipeline) |
| Movie Inspector | audio snapshot: General + Audio details, origin-tagged |
| Dev HUD | `Audio | file | codec srHz ch | dur | F:n/m @ 24fps nominal | Seconds | clk` |

Disabled controls stay visually unchanged — the recorded owner decision from
step 5 applies as-is.

## Verified on screen (2026-08-21, dev box)

- WAV and MP3 open; HUD reads codec/rate/channels/duration; readout defaults
  to Elapsed; frame count = duration × 24 exactly.
- Space plays with sound; playhead locked to the device clock (`F:82` at
  `Seconds 3.417` vs `clk 3.402`, and again after every seek).
- End of stream stops on the last frame; Play at end restarts from 0 (same
  contract as video, including the End-then-Space two-press case).
- Left/Right step the synthetic playhead one frame, paused, readout follows.
- Home / End / Go to Frame land exactly. Go to End was the one real bug the
  test drive found: its lambda computed the target from `frameSource_` and
  silently did nothing on audio — fixed to read the playback state.
- Strip drag while playing: silent, thumb tracks the pointer, release resumes
  playback at the released offset with the clock re-locked. Paused-through-drag
  stays paused.
- Loop on: the run wraps at the end and keeps playing lap after lap.
- Movie Inspector shows the audio snapshot with honest origin tags.

## Regression (2026-08-21, physical panel, both items at HEAD)

- `scrubbar.ps1` full pool: **PASS — 22 files, 88 legs, `delta 0` throughout.**
- 4K H.264 cadence ×2: **99.2/99.1%**, `drop 0`, `rephase 0`,
  `handler>budget 0 of 120`, buckets `~1x 118 / 1.5-2.5x 1` — the recorded
  class. (First rep of each run was voided by a foreground denial to the
  harness and re-taken; the valid samples are quoted.)
- 4444 cadence ×2: **99.8/99.8%**, 261 frames, `drop 0`, `0 of 260`.
- 4444 `scrub -SnapRelease`: **`target 261 shown 261 delta 0`** full-res
  planar, `release 21.3ms`, `hitch 0`, `land 0`.
- `transitions.ps1 -All`: **25 of 25 PASS.**
- `lifecycle.ps1`: **93.9% moving / 0% control.**
- `emptystate.ps1`: all four modes PASS on both backends, plus `-Bar` — after
  the harness's own close-mode focus fix (`Focus-Window`; a silent
  `SetForegroundWindow` denial had accused a correct build).
- `uiatree.ps1`: nine named transport controls + MenuBar + five MenuItems, on
  **identical rects to the pixel** across both backends.

## Knobs and notes

- `TRACE_NO_AUDIO=1` makes an audio file **fail to open** (AudioOutput
  declines by env). That is the honest reading — an audio file with audio
  disabled is nothing — and the message says so.
- Files that state no duration are refused at open rather than given an
  invented length.
- Mute and the volume slider work unchanged (device state, not media state).
