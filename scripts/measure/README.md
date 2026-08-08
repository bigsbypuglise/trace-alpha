# Scrub / playback measurement harness

Drives a built `Trace.exe` with realistic gestures and captures the HUD, so
"does this feel different" can be answered with the same numbers each time.
Windows only; PowerShell 5.1.

```powershell
$clip = "D:\media\yourclip.mov"
.\scripts\measure\restart.ps1 -Clip $clip          # always start here
.\scripts\measure\play.ps1 -Seconds 9              # playback run
.\scripts\measure\capture.ps1 -Out playback.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Seconds 1.5           # forward sweep
.\scripts\measure\capture.ps1 -Out fwd.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Backward -Seconds 1.5
.\scripts\measure\capture.ps1 -Out back.png

.\scripts\measure\restart.ps1 -Clip $clip
.\scripts\measure\scrub.ps1 -Reversals             # the correctness gesture
.\scripts\measure\capture.ps1 -Out rev.png
```

`restart.ps1` finds the exe at `build\app\Release\Trace.exe` relative to the
repo; pass `-Exe` to point elsewhere.

## Run the reversal set, not just a sweep

A single smooth drag scores perfectly on every throughput number and still
misses correctness bugs. `2523d77` — a decode error that put "No decodable frame
at target position" on screen — only appeared under **hard direction reversals
held under one continuous press, running into both ends of the clip**. That is
what `-Reversals` does. It is also the useful stress of buffer recycling: a run
turns over several hundred cache evictions.

## Things that will waste an hour if you rediscover them

- **Capture the window at native resolution.** The HUD is unreadable in a normal
  screenshot on a 5120x1440 panel — it downsamples too far. `capture.ps1` uses
  `GetWindowRect` + `CopyFromScreen` for this reason.
- **Restart before locating the groove.** `scrub.ps1` finds the timeline by
  scanning for the track colour RGB(55,55,55), which is the **unfilled** part of
  the track. At the end of a clip the track is fully filled and the scan finds
  nothing.
- **Take the longest run, not the first match.** Window chrome contains runs of
  similar greys; a first-match scan latches onto the title bar and then "drags"
  there. The scan is restricted to the transport band and keeps the longest run.
- **Do not assume a y offset for the transport.** It sits above a HUD whose
  height depends on the media.
- **Spin, don't sleep, between pointer moves.** A synthetic drag that teleports
  the pointer and pauses overstates how well the shuttle keeps up.

## What to read off the HUD

| field | meaning | expected |
|---|---|---|
| `presented X / Y fps (N% real time)` | playback rate | ~99% on the 1080p clip |
| `rep` / `skip` | frames held / dropped | skip **0**; rep a few per 10s |
| `scrub exact target/shown/delta` | frame identity during a drag | `delta 0` |
| `shuttle Nms/f lag Nf` | drag throughput, frames behind pointer | `lag 0f` when settled |
| `smooth gap ... stalls N of M` | paint interval; stalls are the stutter | fewer is better |
| `rev-hit` | frame cache hit rate | 91–94% backward at 1080p |
| `detach` | conversion deep-copies | **0.00**, structurally |
| `stale-blocked` / `recov` | stale-frame and recovery-seek backstops | **0** |
| `renderer` | which backend is presenting | `cpu` today |

`delta`, `detach`, `stale-blocked` and `recov` are correctness figures — if any
of them moves off its expected value, stop and investigate rather than tuning.
Throughput and `lag` say how many frames were produced and how far behind the
pointer the picture is; they say nothing about *when* frames landed, which is
what smoothness is. That is what `smooth`/`stalls` is for.
