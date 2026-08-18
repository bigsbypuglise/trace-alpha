## Trace v0.3.0-beta.1 — DRAFT, NOT CUT (version agreed 2026-08-18)

> **This is a proposal, not a release.** Nothing is tagged. The version number, the notes and
> whether to cut at all are the owner's call. See the checklist at the bottom for what has to
> happen first.

**The interface was rebuilt.** Testers running `v0.2.0-beta.1` are looking at the old chrome, the
old floating transport panel and a diagnostics HUD that ships visible. None of that is what Trace
looks like now. The playback engine underneath is unchanged and re-measured flat against it.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### What's new

**The window is the picture.** The menu bar and the status bar are out of the layout and the
diagnostics HUD ships hidden. What is left is media, edge to edge — the window reports zero
pixels of chrome on every shape. Every opening size moved as a result, and three of the four
media shapes now reach the design's own area cap instead of being cut off by the work-area
bound: a square clip opens at 960×960 where it used to open at 774×774.

**One transport, across the bottom of the window.** The 460×84 floating panel is replaced by a
56px strip spanning the full width: go to start, rewind, play/pause, fast-forward, go to end,
mute, loop, then the timeline between its two readouts, then fullscreen and share. Ten controls
where there were four. The timeline is a *picture* of the existing slider rather than a rewrite
of it, so exact release, latest-target-wins and the drag shuttle are inherited unchanged —
a scrub release still lands on the frame you pointed at, measured `delta 0`.

**Menus float over the top of the picture and get out of the way.** The menu bar lives in a strip
that appears and hides on the same 2-second reveal the transport uses. It stays a real menu bar,
so `Alt`+letter still opens each menu — including while the strip is hidden — and a screen reader
still finds all five menus and every command in them.

**The strip blurs the video behind it, and respects your Windows setting.** With transparency
effects on in Windows Settings, the top strip draws a blurred copy of the picture it covers under
the design's own scrim. Turn transparency effects off and it falls back to a solid colour, which
is what the design specifies for that case. Cost was measured on the three files that bound the
set, including 4K 60fps against a 16.67ms budget, with the strip held on screen for the whole run
— flat on all of them.

**An empty window now says what it is.** No media gives you the Trace mark and a one-line hint,
centred in the stage, instead of a black rectangle.

**Windows 11 typography.** The application uses Segoe UI Variable at the size Windows is
configured for, with a palette and popup-menu surface taken from the design package.

**Fixed: confirmations were invisible.** "Copied frame", "File path copied" and the rest were
being drawn underneath the new top strip, so the message fired, was correct, and could not be
read for the first two seconds of the two and a half it exists. Found by the both-backend
validation pass.

**Fixed: a disabled control still took its click.** With no media open, clicking Loop on the
empty-state strip turned it on and wrote the preference to disk — from a control that was
supposed to be unavailable.

**Fixed: changing the time-readout mode left the strip showing the old one.** The menu ticked,
the diagnostics line updated, and the two readouts on the transport did not.

### What did not change

Playback, scrubbing, stepping and reverse are the same engine, re-measured against a control
binary built from the previous commit and run beside every leg. 4K H.264 and 4K 60fps hold
99.1–100.0% of real time, ProRes 4444 99.8%, with no dropped frames and no handler over budget;
scrub release lands exactly; the 25-case transport transition matrix passes.

Both renderers were validated against each other. With the interface on screen and no media, the
entire window is **pixel-identical** between the GPU and CPU backends — every control, the
timeline, both readouts, the menu bar, the mark and the hint. `TRACE_RENDERER=cpu` remains the
escape hatch and is still the first thing to try if the picture ever looks wrong.

### Known gaps

- **The frameless window is not built.** Trace still uses the standard Windows title bar. This is
  deferred deliberately — it would put two more messages into the same code path as the aspect
  lock and the DPI reshape, both of which are signed-off geometry.
- **The empty-state mark does not animate.** The design shows a slow gradient roll; the mark
  ships as a static image.
- **The fullscreen strip is the windowed strip.** The design shows a different, taller strip with
  file metadata in fullscreen. Shipping one strip keeps the menus reachable there, which is
  strictly more functional; the second layout is a deliberate omission.
- **8K ProRes 4444 XQ does not reach real time** and is not expected to on this decoder. Best
  full-quality playback is about 57% of real time.
- **EXR does not open.** OpenImageIO is not in the build.
- **Two experimental knobs ship off**: `TRACE_PLAYBACK_QUEUE` (worth ~10% on 8K, worse than off
  at depth 1) and `TRACE_IO_READAHEAD` (correctness-verified but never measured against real
  remote storage).

### Before this can be cut

1. ~~**Owner decides the version.**~~ **DONE — `v0.3.0-beta.1`** (owner, 2026-08-18). The minor
   moved rather than the patch because the shipping window is unrecognisable from the last
   release. `CMakeLists.txt` now says `0.3.0`; the stage did **not** move, so all three "beta"
   literals in `MainWindow.cpp` are correct as they stand. Verified against the built binary
   rather than the source: `0.3.0` present, `0.2.0` absent, and all three literals present.
2. **Push and confirm CI green.** The tag build's five verification
   steps must be read individually, not taken off the summary — in particular `fellback=0`, which
   is the only line separating a real GPU pass from a WARP one.
3. ~~**One Windows setting flip.**~~ **DONE — closed on hardware** (owner, 2026-08-18). With
   transparency effects off the HUD reads `backdrop off (windows)` and the strip is byte-identical
   to the forced-off fallback; forcing it back on with `TRACE_STRIP_BACKDROP=1` against a Windows
   "off" works, which is the override in both directions; and flipping the setting while Trace was
   **running** updated the strip with no restart. The setting has been restored.
4. **An owner look at the new interface on the machine.** Every figure in this release is
   measured; nobody has signed off how it *feels*. The last five interface phases were each
   signed off by eye as well as by number, and this is a bigger visual change than any of them.
