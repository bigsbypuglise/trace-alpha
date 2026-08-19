## Trace v0.3.0-beta.1

**The interface was rebuilt, then reviewed and corrected by the owner.** Testers running
`v0.2.0-beta.1` are looking at the old chrome, the old floating transport panel and a
diagnostics HUD that ships visible. None of that is what Trace looks like now. The playback
engine underneath is unchanged and re-measured flat at every step.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### What's new

**The window is the picture.** The menu bar and the status bar are out of the layout and the
diagnostics HUD ships hidden (`H` brings it back). What is left is media, edge to edge — the
window reports zero pixels of chrome on every shape. Every opening size moved as a result, and
three of the four media shapes now reach the design's own area cap instead of being cut off by
the work-area bound: a square clip opens at 960×960 where it used to open at 774×774.

**One transport, across the bottom of the window.** The floating 460×84 panel is replaced by a
56px strip spanning the full width: rewind, play/pause, fast-forward, mute, loop, the timeline
between its two readouts, then fullscreen and share. The timeline is a *picture* of the existing
slider rather than a rewrite of it, so exact release, latest-target-wins and the drag shuttle
are inherited unchanged — a scrub release still lands on the frame you pointed at, measured
`delta 0`. Go to Start and Go to End live on `Home` and `End` rather than taking two buttons.

**Menus float over the top of the picture and get out of the way.** The menu bar lives in a
strip that appears and hides on the same 2-second reveal the transport uses, and both now fade
in and out smoothly instead of popping. It stays a real menu bar, so `Alt`+letter still opens
each menu — including while the strip is hidden — and a screen reader still finds all five
menus and every command in them.

**The top strip is translucent at rest, and respects your Windows setting.** Settled over the
picture, the strip shows the real video through itself. The resting opacity came from a
legibility sweep over the brightest, most saturated footage in the test set — every menu label
stays separable on the worst frame. Turn transparency effects off in Windows Settings and the
strip goes solid, which is what the design specifies for that case; flipping the setting while
Trace is running takes effect immediately.

**An empty window now says what it is.** No media gives you the Trace mark and a one-line hint,
centred in the stage, instead of a black rectangle.

**Windows 11 typography.** The application uses Segoe UI Variable at the size Windows is
configured for, with a palette and popup-menu surface taken from the design package. Readout
digits are tabular, so the frame counter no longer shivers as it counts.

**Seventeen owner-review fixes landed after the rebuild.** The ones a tester would notice:

- The idle chrome no longer blinks. A Windows-generated synthetic mouse event was re-revealing
  the strip every ~2 seconds with the pointer parked; a move that does not move is no longer
  input.
- `Ctrl+O` (and every other menu shortcut) works while the chrome is hidden.
- Launching from a shortcut no longer opens a console window behind the app.
- Clicking the picture activates the window, so click-then-Space works from cold.
- Loop starts off every session. It still holds across a file change within one.
- The timeline thumb no longer pops to a different size mid-drag, and the track ends are
  rounded.
- Opening a new file keeps the window's top-left corner where it was instead of re-centring.
- The open/close confirmations are gone; Copy Frame's confirmation stays.

**Fixed: confirmations were invisible.** "Copied frame" and the rest were being drawn
underneath the new top strip — the message fired, was correct, and could not be read for the
first two seconds of the two and a half it exists. Found by the both-backend validation pass.

**Fixed: a disabled control still took its click.** With no media open, clicking Loop on the
empty-state strip turned it on and wrote the preference to disk — from a control that was
supposed to be unavailable.

**Fixed: changing the time-readout mode left the strip showing the old one.**

### What did not change

Playback, scrubbing, stepping and reverse are the same engine, re-measured at every step of the
redesign — most steps against a control binary built from the previous commit and run beside
every leg. 4K H.264 and 4K 60fps hold 99.1–100.0% of real time, ProRes 4444 99.8%, with no
dropped frames and no handler over budget; scrub release lands exactly; the 25-case transport
transition matrix passes.

Both renderers were validated against each other, twice — once when the redesign closed and
again after the feedback pass. With the interface on screen and no media, the entire window is
**pixel-identical** between the GPU and CPU backends — every control, the timeline, both
readouts, the menu bar, the mark and the hint. `TRACE_RENDERER=cpu` remains the escape hatch
and is still the first thing to try if the picture ever looks wrong; one deliberate difference
there is that the top strip stays solid rather than translucent on that path.

### Known gaps

- **The frameless window is not built.** Trace still uses the standard Windows title bar, so
  there are two bars at the top when the chrome is revealed. This is deferred deliberately — it
  would put two more messages into the same code path as the aspect lock and the DPI reshape,
  both of which are signed-off geometry. It is the next major interface item.
- **8K ProRes 4444 XQ does not reach real time and is not expected to on this machine and
  decoder.** Best full-quality playback is 13.64 fps (57% of 23.976) with every frame
  presented. This is understood, not mysterious: decode alone is 94% of the frame budget and
  flat from 32 threads to 64 — the limit is per-core throughput, not parallelism, and a
  standalone FFmpeg benchmark reaches the same ceiling with every other stage deleted.
- **EXR does not open.** OpenImageIO is not in the build.
- **HDR and wide gamut are not colour-managed.** BT.2020 content gets the right matrix but no
  tonemap, so HDR/PQ material will look wrong. There is no 10-bit display output; that work is
  gated on a confirmed 10-bit display and a defined colour-management workflow.
- **Mixed-monitor DPI is validated at 100% and 150% only.** Other scale factors, three-plus
  displays, hot-plug and live scale changes are a known, accepted gap.
- **The empty-state mark does not animate.** The design shows a slow gradient roll; the mark
  ships as a static image.
- **The fullscreen strip is the windowed strip.** The design shows a different, taller strip
  with file metadata in fullscreen; shipping one strip keeps the menus reachable there, which
  is strictly more functional. A deliberate omission.
- **Two experimental knobs ship off**: `TRACE_PLAYBACK_QUEUE` (worth ~10% on 8K, worse than off
  at depth 1) and `TRACE_IO_READAHEAD` (correctness-verified but never measured against real
  remote storage).
