# The scrub pass bar — part 3 of the reliability phase

2026-08-20, dev box, the session after parts 1 and 2 (`docs/scrub-population-sweep.md`,
`docs/scrub-gop-sampling.md` — this document assumes both). Part 3 turns the sweep into a
**verdict**: one command, one PASS/FAIL, re-runnable after any scrub change. "MP4s scrub well"
is now checkable rather than felt, which is what the phase charter said closes this
permanently.

## The command

```
scripts\measure\scrubbar.ps1
```

Sweeps the full pool (22 files × 4 legs, ~20 min, `--scrub-selftest` through
`scrubsweep.ps1`, plain config), checks every leg against the committed per-file bar, prints
PASS/FAIL per file with the offending metric named, and exits 0 or 1. `-Csv <results.csv>`
re-verdicts an existing sweep CSV without re-running. `-DisableGopSample` runs the negative
control (below). **Not a CI step** — it needs the real media pool and a real desktop, like
every `--scrub-selftest` use.

## The bar is derived, never hand-maintained

`scripts/measure/scrub-pass-bar.csv`, committed beside the script, one row per (file, leg) —
88 rows. It is **generated** from reference sweep CSVs
(`scrubbar.ps1 -Generate -Csv a.csv,b.csv`), the same reason `verify_trace_assets.py` derives
its set from the `.qrc`: a hand-listed expectation goes stale on the first population change.
The check is two-way — a (file, leg) in the bar but missing from a run FAILS, and a run row
not in the bar FAILS with "population changed — regenerate the bar" — so **adding a file to
`scrubsweep.ps1`'s pool turns the verdict red until the bar is regenerated, on purpose.**

This bar was generated from three fresh fix-config sweeps on `d9a7e37` (binary proven at
HEAD by an incremental build that relinked nothing): **two at 240Hz and one at 60Hz** (via
`setrefresh.ps1`, restored and verified at 239.999 afterwards), per-cell max across all
three — so the bar holds on both displays. The one large display divergence in the pool is
TheraTears leg 1 (`p2p_end` ~19ms at 240Hz vs ~79ms at 60Hz in part 1's data), which is why
one display's figures alone would produce a bar the other display fails. The reference
sweeps read the part-2 record within variance (Universe leg 2 `p2p_end` 56.7/55.0 vs part
2's 57/44.6; Jeep 144.5/87.3 vs 152/149.4; WeLo 73.3/116.3 vs 85/87.9).

**The first two-sweep bar flaked on its own pass-proof, and the third sample plus a
decode-scaled floor is what fixed it — recorded because it is the bar's real design
lesson.** A fresh fix-config sweep read Universe leg 2 at 130.3ms against a bar of 87 (two
reference samples had read 55–57; the file's converged-end state genuinely spans 44–130
across five observed fix runs) and the 8K XQ plate's leg 4 at 51.5ms against a bar of 35
(reference samples ≤4ms — but the plate decodes previews at ~17 f/s, so its end state
inherently varies by ±1 decode interval, ~60ms, and a flat 30ms floor is smaller than the
file's own granularity). Both are the flake class this bar must not have. The fixes: fold
the third sweep into the reference set, and scale the `p2p_end` floor by the cell's worst
observed decode interval.

## What is barred, and what deliberately is not

Per (file, leg): the row must exist, the selftest must exit 0, the leg must read PASS,
**`delta` must be exactly 0** (hard-coded in the script, not stored in the data — it is the
exactness contract, not a tuned expectation), and `behind_end` / `p2p_end` must be at or
under the bar. Those two are the class fix's own headline terms and measured stable.

**Deliberately not barred**: `p2p_max` and `behind_max` (the recorded reversal-gesture
artefact — `p2p max` reads ~1.6s on healthy files across five configs on two machines),
`hitch` (display- and machine-class-dependent), `supply` (a ratio of whatever demand the
gesture generated). A bar on a noisy metric produces flaky failures, which destroys trust in
the bar faster than having no bar at all.

**Margins**: `behind_end` bar = measured + max(4 frames, 15%); `p2p_end` bar = measured +
max(30ms, 15%, **2 decode intervals at the cell's worst observed decode rate**). The 30ms
floor absorbs noise near zero (healthy files read 0–15ms); the decode-interval term is the
pass-proof lesson above — `p2p_end`'s granularity is the file's own decode step, ~11ms on
1080p H.264 and ~120ms on the 8K plate. The 15% is deliberately tight because the margin
must separate the boundary file: **Jeep 4K60 leg 2 reads 144–160ms fixed (three 240Hz
samples) against 199.6–210ms on the control (four samples, two sessions) — the two
distributions are 39ms apart at their closest observed points, and the bar landed at 191,
splitting the gap.** Jeep decodes at ~144 f/s, so the decode-interval term stays under the
30ms floor there and cannot widen it past the control. **Jeep leg 2 is the known flake-risk
row**: if it ever fails alone, with WeLo and Universe passing, read it as boundary-file
variance and re-run before believing it; WeLo or Universe failing is decisive (they clear
their bars by 11x and 1.5x on a regression). The honest fixes for a Jeep flake are more
reference samples or a faster fix, never a wider margin — a margin past ~199 makes the
negative control pass on that file and the row meaningless.

## The bar was proven able to fail before it was believed

Run with `TRACE_SCRUB_GOP_SAMPLE=0` (the part-2 control, via `scrubbar.ps1
-DisableGopSample`), same binary, same session:

```
FAIL  17_Random_Mp4s\WeLo-AI-rc13-1x1_SOCIAL.mp4
        leg 2: behind_end 216 > 4f, p2p_end 1681.5 > 147ms
FAIL  3_1080p_H264_MP4\Universe_rc07_I_9x16_Online.mp4
        leg 2: p2p_end 245.2 > 161ms
FAIL  7_4k_H264_60FPS_MP4\Jeep_Snowboard_Drone_60FPS.mp4
        leg 2: p2p_end 199.6 > 191ms
SCRUB BAR: FAIL -- 3 of 22 files below the bar
```

**Exactly the three files of part 1's class, exactly on the reversal leg, and the other
nineteen pass** — the verdict detects the regression it exists to detect, and nothing else.
Three checker-level negative controls were also run against a doctored CSV: a `delta 3` on
one leg (FAIL naming the exactness contract), a file removed from the results (4 MISSING
legs), and a file added that the bar does not know (NOT IN BAR). Each exits 1.

The pass-proof: the bare `scrubbar.ps1` end-to-end on the shipping config, on a **fresh
sweep the bar had never seen** (a fourth fix-config run, not one of the three reference
sweeps), reads `SCRUB BAR: PASS -- 22 files, 88 legs, delta 0 throughout`, exit 0 — with the
discriminators well inside their bars (Universe 50.1 vs 161, Jeep 140.2 vs 191, WeLo 89.0 vs
147, all four 8K legs inside).

## Regenerating after an intentional behaviour change

When a scrub change legitimately moves the numbers (or the pool changes), take fresh
reference sweeps — at both displays, and give the generator every clean fix-config sweep you
have; two samples underestimated Universe's spread by 2.3x — and regenerate:

```
scripts\measure\scrubsweep.ps1 -Pass bar-240hz
scripts\measure\setrefresh.ps1 -Hz 60
scripts\measure\scrubsweep.ps1 -Pass bar-60hz
scripts\measure\setrefresh.ps1 -Hz 240        # ALWAYS restore explicitly
scripts\measure\scrubbar.ps1 -Generate -Csv %TEMP%\trace-scrubsweep\results-bar-240hz.csv,%TEMP%\trace-scrubsweep\results-bar-60hz.csv
```

Then **re-run the `-DisableGopSample` fail-proof before committing the new bar** — a bar that
cannot fail proves nothing, and this project has been caught by exactly that four times. The
generator refuses a reference CSV whose legs are not structurally clean (`delta 0`, PASS,
all legs present), so a bar cannot be derived from a broken sweep.

The `src_*` and `sources` columns in the bar file carry the measured values and the CSVs each
row came from, so a future regeneration diffs legibly.
