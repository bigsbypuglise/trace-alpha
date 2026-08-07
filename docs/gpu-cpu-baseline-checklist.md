# Phase 2 — CPU reference baseline

Run **before** any renderer code exists. This becomes the immutable comparison point for
every GPU claim. Measured on `main` at or after `cd79d49`.

> **Do not reuse the numbers in the initiative brief.** They predate the audio-scheduler fix
> (`cd79d49`, skips 7 → 0, 95.1% → 99.1% of real time on 1080p H.264). The baseline must be
> re-measured on current HEAD.

---

## Step 0 — two experiments that come first (5 minutes, no code)

These can change the priority of the whole initiative, so do them before the table.

### 0a. Display refresh rate

The test box reports **5120x1440 @ 59 Hz** on a card whose `MaxRefreshRate` is **240**.

At 59 Hz a 24 fps source *cannot* have equal frame durations — it needs an irregular 2-3
cadence forever. At 120 Hz, 24 fps is exactly 5 refreshes per frame; at 240 Hz, exactly 10.

1. Play the 1080p H.264 clip at **59 Hz**. Note how it feels.
2. Set the display to **120 Hz** (Windows Settings → System → Display → Advanced display).
   Play it again.
3. Set it to **240 Hz** if the panel allows at this resolution. Play again.

Record a plain subjective verdict for each. **If the judder largely disappears at 120/240 Hz,
cadence is confirmed as the dominant term** and the presentation-clock work (Phase 13) is the
real payoff — which reorders the initiative. If it does not change, we have eliminated a whole
hypothesis for five minutes' work.

### 0b. Confirm the viewing path

There is a **Parsec Virtual Display Adapter** on this machine.

- Was any of the "not as buttery as QuickTime" feedback given over **Parsec, RDP, or any
  remote session**? Remote desktop re-encodes and re-times the stream; smoothness feedback
  through it is not evidence and must be re-taken at the physical machine.

Answer yes/no. If yes, re-take that feedback locally before we optimise against it.

---

## Recording rules

Every row must record these or runs are not comparable:

- **display refresh rate** at the time of the run
- **session type** — local console / Parsec / RDP
- **window size** and whether the HUD was visible
  (hiding the HUD resizes the viewer and changes the draw path — this bit us already)
- whether the file **has an audio track**

Read numbers off the dev HUD. Let each clip play to its end; the counters freeze on stop.

## Files

| # | file | path |
|---|---|---|
| 1 | 1080p H.264 | `Trace_Testing_Assets/3_1080p_H264_MP4/Marinelaverse_16x9_EndTag_v007.mp4` |
| 2 | 4K H.264 | `Trace_Testing_Assets/4_4K_H264_MP4/Splash_1.mp4` |
| 3 | 4K ProRes 422 HQ | `Trace_Testing_Assets/2_4K_ProRes_422HQ/Barritas_16x9_Shot_040-080_v005_ALT_1.mov` |
| 4 | 4K ProRes 4444 | `Trace_Testing_Assets/1_4K_ProRes_4444/TheraTears_Vial_VFX_v002.mov` |
| 5 | vertical 1080x1920 | `Trace_Testing_Assets/3_1080p_H264_MP4/Universe_rc07_I_9x16_Online.mp4` |
| 6 | heavy downscale | file 3 or 4, window shrunk so `display` shows **>2x** below source |
| 7 | representative local | any current job file |
| 8 | LucidLink warm | *optional* — a file you nominate on `V:\`, read-only |

## Per-file: playback

Play each to the end, then read the HUD.

| metric | HUD field |
|---|---|
| decode ms (last/avg) | `dec` |
| swscale ms (last/avg) | `sws` |
| prep / handoff ms | `alloc`, `memcpy`, `handoff` |
| paint ms | `paint`, `draw`, `upd->paint` |
| total per-frame | `total` vs `budget` |
| actual fps / target fps | `presented X / Y fps (Z% real time)` |
| span fps | `span-fps` |
| presentation interval + variance | `period last/avg/max` |
| **worst-case interval** | `period` **max** — record it; averages hide the hitch people see |
| present-late | `present-late last/avg/max` |
| repeated / skipped | `rep` / `skip` |
| A/V corrections | `sync`, `under`, `clk-upd` |
| audio buffer geometry | `audiobuf` line |
| cache | `hit %`, `ins`, `evict`, occupancy |
| renderer scaling state | `display WxH` + `1:1` / `filtered` / `NEAREST` |

Also capture, outside the HUD:

- **CPU usage** during playback (Task Manager → Details → Trace.exe)
- **peak working set** (same place)

## Per-file: scrub

Three passes per file. The HUD's `scrub` line gives `target` / `shown` / `delta` / `walk`.

1. **Cold random scrub** — reopen the file, then click 5 spread-out points on the timeline.
   Record for each: `walk` frames, `seek` ms, cache `hit %`, and how long until the picture
   settles.
2. **Warm scrub** — play the file through once, then repeat the same 5 points. Record the same
   fields. (Expect a large hit-rate improvement; playback warms the cache.)
3. **Rapid back-and-forth drag** — grab the slider and sweep left-right continuously for ~10 s.
   This is the case the async work targets. Record subjectively: does the playhead keep up with
   the pointer, and how far behind does the picture run?

Then, for release-to-exact latency:

4. Drag to a point, release, and note the time until `scrub exact | delta 0` shows the released
   target at full res.

## What "good" looks like today

For calibration, measured on 2026-08-07 at `cd79d49`, HUD visible, 59 Hz, local console:

| file | frames | real time | rep | skip | drift |
|---|---|---|---|---|---|
| 1080p H.264 (`topgun`, 241f) | 240/240 | 99.1% | 4 | 0 | −87 ms |
| 4K H.264 | 120/120 | 98.3% | 2 | 0 | −85 ms |
| 4K ProRes 422 HQ | 168/168 | 98.4% | 3 | 0 | −115 ms |
| 4K ProRes 4444 (no audio) | 261/261 | 98.3% | — | — | −188 ms |

Per-frame cost on 1080p H.264: decode 0.01–0.17 ms, sws 0.53–0.71 ms, paint 0.34 ms,
**total ~0.8–1.2 ms against a 41.67 ms budget.**

That is the point of the whole initiative: **there is no CPU throughput problem left to
solve.** Anything GPU work improves has to show up in cadence, scaling quality, or navigation
latency — not in these numbers.
