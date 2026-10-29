## Trace v0.4.0-beta.2

**A frame-accuracy fix for footage straight off a phone, and the first build meant to go on
several machines at once.**

Trace could show you the wrong frame and tell you it was the right one. Not on every file — on
material encoded with *open GOPs*, which is what iPhones and most cameras write. Asking for a
particular frame could land one or two frames late, and every number Trace reported about the
landing said it was exact. That is fixed.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### The frame-accuracy fix

- **A seek that lands past the frame you asked for now seeks further back, instead of relabelling
  what it found.** On open-GOP footage the handful of frames just before a keyframe cannot be
  decoded after a seek to that keyframe, so the first frame out is the keyframe itself — later
  than the one you wanted. The old code quietly renamed it. On a test clip, **Go to Frame 286
  displayed frame 288** while the diagnostics read `delta 0`.
- **It was found by burning the frame number into the picture of real iPhone footage and reading
  it back off the screen**, rather than by asking Trace whether it agreed with itself. The
  synthetic test files this had been checked against could not have found it: their keyframes are
  250 frames apart, so about 1% of frames are affected, against about 12% on phone footage.
- **If a file ever defeats the fix, Trace now reports a non-zero `delta`** instead of claiming
  exactness. It may show you the wrong frame when a file beats it. It may not call it the right
  one.
- Checked for regressions across the whole 18-file synthetic corpus and both iPhone clips —
  **17 navigation legs per file, every landing read from the picture, zero mismatches.**

### Smaller fixes

- **Go to Frame refuses an out-of-range number instead of silently truncating it.** Typing `900`
  on a 480-frame clip used to leave `90` in the box — the third keystroke was swallowed by the
  field — and the playhead went to frame 90. It now says the frame is outside the media and does
  not move.
- **The diagnostics say when the fast GPU upload path has stood down.** Loading a LUT puts Trace
  on a slower colour path and nothing on screen said so. The HUD's first line now reads
  `planar on`, or `planar off (color)` when a colour transform is the reason.
- **The issue report carries your GPU and the decode path**, so a report answers those two
  questions without anyone having to ask.

### For testers

**This build is going to several machines on purpose.** What is useful back:

- **Your Windows version, your GPU, and every monitor's resolution, refresh rate and scale
  factor.** All of them, not just the main display.
- **Which launcher you used.** This release is software decode only and the report will say
  `Decode: software`; a later build is expected to add a hardware-decode launcher to A/B against.
- **A screenshot of the diagnostics HUD.** Press `H` to show it. Nearly every question about
  playback, scrubbing or audio is answered by that one block of text, and it is the only window
  anyone here has into a machine they cannot sit in front of.
- **Mixed-DPI setups especially.** If you run two or more monitors at *different* scale factors —
  100% on one and 125%, 150% or 175% on another — that is the oldest untested thing in this
  project, and your machine is the first real chance to check it. Drag the Trace window between
  the monitors, resize it on each, go fullscreen on the secondary one and press Escape. Worth
  reporting: the picture no longer filling the window, the window coming back the wrong shape, or
  the proportions looking wrong after a move.
- **Open a clip straight off your phone and step through it.** The fix above is about exactly that
  material. Use the arrow keys and Go to Frame (`Ctrl+G`), and say if any frame looks like it is
  not the one you asked for.

Help ▸ Report an Issue opens a pre-filled mail with the build identity, the GPU and the decode
path already in it.

### Known and unchanged

- **A file whose container reports a wildly wrong nominal frame rate can be partly unreachable.**
  Trace maps "frame N" to a timestamp using the rate the container states; if that rate is a large
  multiple of the real one — which happens when frame durations are irregular, as some screen
  capture and AI export pipelines produce — then Go to Frame and the End key can only reach the
  first part of the clip. Measured on a deliberately pathological test file whose stated rate is
  five times its real one. No real-world file in testing has this shape, and the clips it would
  affect are identifiable: if a clip's duration and frame count do not agree with its stated frame
  rate, this is the case. Known, understood, not fixed in this release.
- **Heavy multilayer EXR sequences do not play at full real time.** A 27-channel multilayer DWAA
  sequence at 1920×1080 plays at roughly **15 frames per second — about two-thirds of real time**.
  **This is considered acceptable for review on this difficult file class, and it is not full
  real-time playback.** The limit is the file rather than anything schedulable: reading a single
  frame takes about as long as the entire frame budget at 24fps.
- **Single-layer EXR and PNG sequences are unaffected and hold real time** — 99.9% and 100.0%,
  with no frames skipped.
- **The first play of any EXR sequence is roughly half speed** while Windows fills its file cache.
  Press Home and play it again; the second pass is the real figure. This is the operating system,
  not Trace.
- **8K ProRes 4444 XQ does not reach real-time playback**, and this is understood rather than an
  open bug: best measured is **13.64 fps (56.9% of real time)** at full quality with every frame
  shown, decode-bound at the CPU's own ceiling for that codec. No further work is planned here.
- **Very high-bitrate media will not stream cold from LucidLink.** Cold remote delivery measured
  around 600–800 Mbps, so a multi-gigabit-per-second plate cannot play at real time from a cold
  cache. Once warm it plays at whatever the local CPU can do.
- **A small window-position drift on multi-monitor setups with different display scaling**: going
  fullscreen and back on a secondary monitor at 150% can land the window about 7 pixels higher
  than it started. Size is unaffected. Known, not yet patched.
- **The title-bar freeze is unchanged and closed as understood, not fixed.** Pressing and holding
  the real Windows title bar freezes the app for about half a second before the window starts
  moving. It is Windows itself, and fixing it would mean reimplementing the title bar by hand,
  taking Snap, Aero Shake and multi-monitor drag with it.
- 10-bit output is still deliberately not in this build.
- HDR/PQ material gets the right colour matrix but no tonemap.
- Audio during scrubbing, reverse playback and off-speed (J/L) playback is deliberately silent.

### Rollback knobs for this release

| knob | effect |
|---|---|
| `TRACE_SEEK_OVERSHOOT_RESEEK=0` | the seek fix off, back to the previous behaviour — the A/B for anything that feels different about stepping or Go to Frame |
| `TRACE_RENDERER=cpu` | the software renderer — first thing to try if the picture looks wrong |
| `TRACE_SEQ_PREFETCH_STRIDE=0` | image-sequence prefetch back to the fixed one-frame-either-side window |
| `TRACE_VOLUME_SLIDER=0` | mute-only button, no slider, no stored level |
| `TRACE_FS_MAG_FILTER=0` | fullscreen magnification back to the sharp sampler |
| `TRACE_MARK_ANIM=0` | empty-state mark held still |
| `TRACE_SCRUB_PAINT_GATE=0` | the beta.3 scrub paint gate off |

### Other knobs, if you are testing something specific

| knob | effect |
|---|---|
| `TRACE_COLOR_LUT=<path>` | load a LUT at startup without going through the dialog |
| `TRACE_COLOR_VIEW=<config>` | configure a display/view transform at startup; optionally `<config>\|<input>\|<display>\|<view>` |
| `TRACE_SEEK_LOG=1` | one line per seek to `%TEMP%\trace_seeklog.txt`, including any overshoot re-seek |
| `TRACE_IO_READAHEAD=1` | read-ahead buffering for remote storage. Correctness-verified, but **not validated against a real remote mount**. |
| `TRACE_PLAYBACK_QUEUE=2` | decode up to N frames ahead on a background worker. Worth roughly +10% on very heavy material; depth 1 is measured *worse* than off. |

### If something is wrong

Help ▸ Report an Issue opens a pre-filled mail with the build identity in it. Press `H` to show
the diagnostics HUD and include a screenshot — nearly every question about playback, scrubbing or
audio is answered by that one line of text.
