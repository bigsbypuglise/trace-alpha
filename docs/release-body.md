## Trace v0.3.0-beta.3

**The MP4 scrub fix.** The application is `v0.3.0-beta.2` plus one engine change: scrub
decode is decoupled from screen painting during a drag, which fixes the machine class where
scrubbing MP4s was pinned to a crawl while playback stayed perfect. No interface changes.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### What was wrong

The `beta.2` diagnostic run on the affected machine (Threadripper 3970X) answered the
question. Scrubbing was not decode-bound — a 1080p H.264 frame decodes there in under a
millisecond — and not thread-bound. It was **paint-bound**: on that machine every present
blocks until the display consumes a frame, so the scrub chain, which painted once per decoded
frame, was capped at the monitor's refresh rate. Sixty frames a second of picture against
three hundred frames a second of hand. Playback never showed it because playback only asks
for 24 frames a second.

That blocking behaviour is a property of the machine's display driver / presentation mode,
not of the refresh rate itself — the development box at a true 60Hz does not block and never
showed the fault, which is why it took the diagnostic to find.

### The fix

During a drag, every frame is still decoded, in order, exactly as before — nothing is
skipped and the frame you release on is still landed exactly, at full resolution. What
changed is that the *screen* is now updated at most once per display refresh (or less often
if painting itself is measurably expensive on the machine), always with the newest decoded
frame. Painting faster than the display can show was never visible; on the affected machine
class it was actively starving the scrub loop.

On the fault model this took scrub delivery from 3–12% of the demanded rate to 100%+, with
the release still landing frame-exact. On healthy machines the change is measured flat
across the entire regression suite — scrubbing, playback cadence, lifecycle and all 25
transport transitions.

### Verifying it on the machine that had the problem

Just scrub an MP4 hard — fast sweeps, quick back-and-forth. It should now track the pointer.

If you want numbers, the diagnostic gained a fourth leg (rapid back-and-forth) and new
fields; run it plain and send back the file it writes beside `Trace.exe`:

```
Trace.exe "--scrub-selftest=C:\path\to\some-clip.mp4"
```

Expected on the previously-affected machine: `supply` near 100% on every leg, `paint cost`
reading about one refresh (~16.7ms at 60Hz), and `delta 0`.

If anything about scrubbing reads or feels *worse* on any machine, this restores the previous
behaviour for comparison:

```
set TRACE_SCRUB_PAINT_GATE=0
Trace.exe
```

### Everything else

Unchanged from `v0.3.0-beta.2`. Known gaps are the same: 8K ProRes 4444 XQ does not reach real
time and is understood rather than solved; EXR does not open; HDR / BT.2020 has no tonemap;
there is no 10-bit output path; mixed-monitor DPI is validated at 100% and 150% only. If
anything about the picture looks wrong, `TRACE_RENDERER=cpu` is the escape hatch.
