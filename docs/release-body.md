## Trace v0.2.0-beta.1

**First beta.** The gate that kept the last release at alpha — mixed-monitor DPI having never
run on real hardware — has been closed on real hardware, and it found and fixed a real bug on
the way. Everything else in this release is playback and responsiveness work.

Still a beta, deliberately, and the *Known gaps* section below says exactly what is missing.
It is shorter than last time and every entry in it is now measured rather than suspected.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by
design (see *Packaging* below).

### What's new

**Windows across two monitors at different scaling now works, and did not before.** A window
dragged from a 100% display to a 150% one never re-ran its sizing pass, so it came off the
crossing the wrong shape with the picture pillarboxed inside a window built to have no bars —
147 logical pixels of height lost, and the window left larger than the work area. Fixed, and
verified on hardware in both directions: the round trip now returns to the opening geometry
exactly, repeatedly. Fullscreen on the secondary display, maximize, snap, the aspect lock
under an interactive corner drag at 150%, opening media on the secondary, and playback across
a live scale change were all checked on the same pass.

**Scrubbing is roughly 11× more responsive on short, cheap, frame-dense files.** The kind of
clip an AI video tool exports — 720p or 1080p, 10–20 seconds, 24fps — was the one shape where
each frame is so cheap to decode that the cost of asking for it dominated: one cross-thread
round trip per frame, 97% of the time spent in the round trip. One request now covers a
bounded run of **consecutive** frames. Nothing is sampled and nothing is skipped — every frame
is still decoded, delivered and presented individually and in order. On the reported file the
picture went from trailing the pointer by up to 93 frames to 6, and pointer-to-picture latency
from 292ms to 26ms. Heavy media is unaffected by construction: a time budget collapses the
batch to one frame, so ProRes 4444 behaves exactly as before.

**Clicking the timeline, releasing a drag, and stepping a frame no longer freeze the window.**
That decode moved off the UI thread. On a 4K clip whose whole 97 frames sit in a single GOP —
so showing frame 57 genuinely requires decoding 58 frames, in any player ever written — a
click used to lock the window for 267–589ms depending where you clicked. It is now **0ms
frozen**: the wait is the same wait, but the window stays alive through it. **Exactness is
completely unchanged** — same request, one frame, full resolution, accurate conversion. What
changed is only which thread pays for it.

One consequence worth knowing: **rapid frame steps now coalesce.** Five fast presses of the
arrow key move five frames and decode the fifth. The frame you stop on is exact, as always.
The behaviour it replaces was worse — held arrow keys queued one full decode per press and ran
the playhead away from you after you let go.

**Large ProRes decodes measurably faster.** Intra-only codecs now use the machine's actual
logical CPU count rather than FFmpeg's automatic count, which caps at 16 and left half of a
32-thread machine idle. It is derived from your machine, not hard-coded. It **also helps
random access**, which is the opposite of what you might expect: on 4K ProRes 4444 a scrub
release went from 29.6 to 15.9ms per frame with the hitch count dropping to zero.

**Also**: FFmpeg is now a minimal LGPL build compiled specifically for Trace — 20.8MB of DLLs
instead of 104MB, no encoders or muxers, and about 18% faster at decoding large ProRes ·
playback no longer quantises to a fraction of the frame rate on a source that cannot meet its
budget · the applied decode thread count is visible in the diagnostics HUD (`H`).

### For testers with heavy media

There is one experimental knob in this build, **off by default**, and it exists for exactly one
case: media too heavy to play at its native rate on your machine.

Set `TRACE_PLAYBACK_QUEUE=2` in the environment before launching to let playback decode up to
two frames ahead on a worker thread. On an 8K ProRes 4444 XQ plate this is worth about +9%
(56.9% → 62.0% of real time). On any file that already plays at rate it does nothing at all.

**Depth 1 is worse than off** — a depth-1 queue cannot overlap anything — so use 2 or leave it
alone. It ships off because off is the configuration every regression figure in this release
was measured on, and the validated path is the one that ships.

### Known gaps — please read before filing

These are known. Reports about them are not needed; reports about anything else are very
welcome.

- **8K ProRes 4444 XQ does not play in real time, and this is understood rather than unsolved.**
  Best full-quality playback on a 16-core/32-thread machine is **13.64 fps against 23.976** —
  every frame presented, nothing dropped, correct colour. The reason is that **decode alone
  consumes 94% of the frame budget** (39ms of 41.7ms) before any conversion, upload or paint,
  and it is already at its threading knee: the curve is flat from 32 threads to 64, and the CPU
  never exceeds half the machine at any setting. This is per-core throughput, not a missing
  optimisation. A perfect fully-pipelined version of Trace would reach about 25.6 fps — a 6%
  margin — which is why that work has not been done. **4K ProRes 4444 and below are fine.**
- **EXR does not open.** OpenImageIO is not built into this package, so `TRACE_WITH_OIIO` is
  undefined. EXR and OCIO display transforms are future work.
- **HDR and BT.2020 content will look wrong.** The correct matrix is applied but there is **no
  tonemap**, on either renderer. PQ/HLG material is not usable in this build.
- **10-bit display output is not supported.** Output is 8-bit SDR. This is *not* the same thing
  as high-bit-depth processing, which does work — 10-bit and 12-bit sources are decoded and
  converted at their real bit depth; it is the final output that is 8-bit.
- **Multi-monitor DPI is validated at 100% and 150% only.** Two displays, both directions,
  fullscreen and maximize on each — that ran on hardware and is what closed the beta gate.
  What has **not** been exercised is three or more displays, scale factors of 125% or 175%, a
  scale factor changed in Windows Settings while Trace is running, and display hot-plug. If you
  work in any of those configurations, that is where to look and a report is genuinely useful.
- **LucidLink cold reads deliver roughly 600–800 Mbps.** Files above that bitrate will not play
  in real time from a cold cache no matter how they are buffered. Warm playback is normal. 4K
  ProRes 422/HQ-class media is the realistic target. There is an experimental read-ahead in
  this build (`TRACE_IO_READAHEAD=1`) which is **off by default and has never been tested
  against a real remote mount** — it is verified correct and measured only against a simulated
  slow link, so please do not read its presence as a fix.
- **Copy LucidLink Link needs the LucidLink Windows shell extension installed**, and it
  identifies the command by its English display text. On a localized Windows it will report the
  integration as unavailable rather than invoke the wrong menu item — that is deliberate.

### If the picture looks wrong

Set `TRACE_RENDERER=cpu` and relaunch. That selects the CPU renderer instead of the D3D11
default and is the first thing to try for any picture problem — it is slower and slightly
softer, but it is the reference path. Tell us if it fixes what you were seeing; that is the
single most useful thing a report can contain.

### Packaging

Portable ZIP only, by deliberate choice — no installer, no file associations, no auto-updater,
and nothing written to the registry. Delete the folder and Trace is gone. An installer comes
when packaging and playback are stable.

If you place a writable `trace.ini` next to `Trace.exe`, Trace uses it for settings and stays
fully portable. Trace never creates that file itself; its presence is how you ask for portable
mode.

The ZIP asset is still named `trace-alpha-windows-x64.zip`. That is the build pipeline's
artifact name, not a statement about this release; it is left alone here rather than renamed
inside a release commit.

---
