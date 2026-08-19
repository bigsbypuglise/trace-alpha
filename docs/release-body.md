## Trace v0.3.0-beta.2

**A diagnostic point release.** The application is `v0.3.0-beta.1` plus one new capability: a
built-in scrub self-diagnostic. There are no interface changes, no playback changes and no
engine changes — the playback regression was re-measured flat before this was cut.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### Why this build exists

Scrubbing MP4 files is badly degraded on one tester's machine — an AMD Threadripper 3970X, 32
cores / 64 logical — while playback on the same machine is fine, and while the same files scrub
correctly on the development box. It reproduces on a file with every figure already recorded
against it, so it is the machine rather than the media.

Two hypotheses have already been tested and refuted on that machine: capping the decode thread
count, and forcing slice-only threading for long-GOP codecs. Neither helped. The remaining
question needs numbers from the affected machine, and that machine cannot run the measurement
harnesses. So the measurement moved into the application.

### `--scrub-selftest`

From a Command Prompt in the folder holding `Trace.exe`:

```
Trace.exe "--scrub-selftest=C:\path\to\some-clip.mp4"
```

The window opens, scrubs itself for about twenty seconds, and exits. No mouse input is needed
and there is nothing to configure.

It writes **`trace-scrub-report.txt` beside `Trace.exe`** — that one file is the whole
deliverable. It reports the machine's identity and core counts, the applied decoder threading
mode, the scrub worker's round trip split into wake / decode / delivery / overhead, per-frame
decode cost, the batch actually achieved against its cap, cache occupancy and hit rate, seeks,
walk length, and pointer-to-picture lag.

Three legs run against a fresh open of the clip each time — a fast forward sweep, a reversal
drag, and a slow drag where a fixed per-request cost shows most clearly. The window is forced to
a fixed size so two machines measure at the same cache depth, and the gesture is timed rather
than step-counted, so a slower machine shows its deficit as lag rather than slowing the
simulated hand down to hide it.

**Run it plain.** Do not set `TRACE_HUD=1` — the diagnostics HUD itself costs roughly 2.5ms of
delivery latency per request, which is most of the round trip on a healthy machine. The report
prints whether the HUD was shown, and a result is only comparable to another taken the same way.

**Slow numbers are the report, never a failure.** Exit 0 means a report was produced; 6 means
the clip could not be opened; 7 means a leg failed structurally.

### Everything else

Unchanged from `v0.3.0-beta.1`. Known gaps are the same: 8K ProRes 4444 XQ does not reach real
time and is understood rather than solved; EXR does not open; HDR / BT.2020 has no tonemap;
there is no 10-bit output path; mixed-monitor DPI is validated at 100% and 150% only. If
anything about the picture looks wrong, `TRACE_RENDERER=cpu` is the escape hatch.
