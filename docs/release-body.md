## Trace v0.4.0-beta.1

**Trace opens EXR now.** Stills and numbered sequences, read as scene-linear float rather than
flattened to 8 bits on the way in, with OpenColorIO colour management over the top and
multilayer pass cycling. This is the first release where the minor version moved for what Trace
can *open* rather than for how it looks.

The playback engine underneath is unchanged and was re-measured flat at the panel before this was
cut — 22 files, 88 scrub legs, exact frame landing on every one.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.
**The download is larger than previous releases** — about 118 MB unpacked against 95 MB — because
OpenImageIO and OpenColorIO now ship inside it.

### EXR, with colour management and multilayer passes

- **EXR stills and sequences open.** Point Trace at a `.exr` or at a numbered sequence folder and
  it plays, the same as any other media. Values above 1.0 survive into the display stage instead
  of being clipped on load.
- **Colour transforms** through OpenColorIO: load a `.cube` LUT, or pick a config, input colour
  space, display and view from **View ▸ Color Transform…**. **`C`** toggles the transform off and
  on, so you can A/B against the raw image without reopening anything.
- **Multilayer passes.** **`[`** and **`]`** cycle the passes in a multilayer EXR, and
  **View ▸ EXR Pass** lists them by name. Beauty, position, normal, depth and data passes each get
  an appropriate on-screen mapping, and the HUD always says which mapping is in force and what
  range it measured — normalising the picture to make it visible is allowed, doing it silently is
  not.

### ⚠️ Copy Current Frame now copies what is on screen

**This is a deliberate change to existing behaviour, and it affects video as well as EXR.**

`Ctrl+C` used to put the raw source pixels on the clipboard. It now puts **the image you are
looking at**. With a LUT or view transform active you get the graded picture, not the flat log
source.

If you have been using Copy Frame to grab untouched source pixels, that is what changed. It was
changed on purpose: it is the only correct answer for EXR, whose source frame is scene-referred
float with no single right 8-bit reading, and for video it is almost always what someone copying
a frame to send to a colleague actually wants. The rotate/flip view transform is still *not*
applied — that part is unchanged.

### Known and unchanged

- **Heavy multilayer EXR sequences do not play at full real time.** A 27-channel multilayer DWAA
  sequence at 1920×1080 plays at roughly **15 frames per second — about two-thirds of real time**
  (measured 63.6–65.1% of 24fps across four warm passes on the test system). **This is considered
  acceptable for review on this difficult file class, and it is not full real-time playback.**
  The limit is the file rather than anything schedulable: reading a single frame of that file
  takes about as long as the entire frame budget at 24fps, so the read alone is the whole budget
  before a pixel is drawn. Trace already issues almost exactly one read per frame shown, so there
  is no speculative work left to remove. If your work is heavy multilayer EXR at speed, know this
  going in.
- **Single-layer EXR and PNG sequences are unaffected and hold real time** — measured 99.9% and
  100.0% respectively, with no frames skipped.
- **The first play of any EXR sequence is roughly half speed** while Windows fills its file cache.
  Press Home and play it again; the second pass is the real figure. This is the operating system,
  not Trace.
- **8K ProRes 4444 XQ does not reach real-time playback**, and this is understood rather than an
  open bug: best measured is **13.64 fps (56.9% of real time)** at full quality with every frame
  shown, decode-bound at the CPU's own ceiling for that codec. No further work is planned here —
  a faster decoder or GPU decode would be needed, and GPU decode is explicitly out of scope.
  `TRACE_RT_DROP=0` is available if you want to compare against the frame-dropping fallback, but
  that fallback is not the answer and is not going to become the default.
- **Very high-bitrate media will not stream cold from LucidLink.** Cold remote delivery measured
  around 600–800 Mbps, so a multi-gigabit-per-second plate cannot play at real time from a cold
  cache no matter how it is buffered. Once the file is warm it plays at whatever the local CPU can
  do. Ordinary 4K ProRes 422/HQ review material is the case that works.
- **A small window-position drift on multi-monitor setups with different display scaling**: going
  fullscreen and back (Escape) on a secondary monitor running at 150% scaling can land the window
  about 7 pixels higher than where it started. Size is unaffected — this is a small position
  nudge, not the framing bug to watch for. Known, not yet patched. **Real mixed-monitor DPI beyond
  100%/150% remains unvalidated** — other scale factors, three or more displays, and changing a
  monitor's scaling while Trace is running have not been tested on hardware.
- **The title-bar freeze from beta.8 is unchanged and is closed as understood, not fixed.**
  Pressing and holding the real Windows title bar freezes the app for about half a second before
  the window starts moving. It is Windows itself — the UI thread receives no message of any kind
  during it, and it happens before the window-move operation begins. Fixing it would mean
  reimplementing the title bar by hand, and Snap, Aero Shake and multi-monitor drag with it.
- 10-bit output is still deliberately not in this build — it needs a confirmed 10-bit display and
  a defined HDR/colour-management workflow before it's worth building, neither of which is in
  place yet.
- HDR/PQ material gets the right colour matrix but no tonemap.
- Audio during scrubbing, reverse playback, and off-speed (J/L) playback is deliberately silent.

### Rollback knobs for this release

| knob | effect |
|---|---|
| `TRACE_SEQ_PREFETCH_STRIDE=0` | image-sequence prefetch back to the fixed one-frame-either-side window, if the new policy ever misbehaves |
| `TRACE_RENDERER=cpu` | the software renderer — first thing to try if the picture looks wrong |
| `TRACE_VOLUME_SLIDER=0` | mute-only button, no slider, no stored level |
| `TRACE_FS_MAG_FILTER=0` | fullscreen magnification back to the sharp sampler |
| `TRACE_MARK_ANIM=0` | empty-state mark held still |
| `TRACE_SCRUB_PAINT_GATE=0` | the beta.3 scrub paint gate off |

### Other knobs, if you are testing something specific

| knob | effect |
|---|---|
| `TRACE_COLOR_LUT=<path>` | load a LUT at startup without going through the dialog |
| `TRACE_COLOR_VIEW=<config>` | configure a display/view transform at startup; optionally `<config>\|<input>\|<display>\|<view>` |
| `TRACE_IO_READAHEAD=1` | read-ahead buffering for remote storage. Correctness-verified — pixel-identical output against the plain path — but **not validated against a real remote mount**. If you try it on a real `V:\` file, the useful report is whether it visibly helps or hurts: a feel, not a number. |
| `TRACE_PLAYBACK_QUEUE=2` | decode up to N frames ahead on a background worker. Worth roughly +10% on very heavy material (the 8K plate). Depth 2 is the minimum that does anything; depth 1 is measured *worse* than off. Does nothing noticeable on ordinary 4K/1080p media. |

### If something is wrong

Help ▸ Report an Issue opens a pre-filled mail with the build identity in it. Press `H` to show
the diagnostics HUD and include a screenshot of it — nearly every question about playback,
scrubbing or audio is answered by that one line of text. On an EXR sequence the HUD also names
the colour mapping in force, the active pass and the prefetch policy.
