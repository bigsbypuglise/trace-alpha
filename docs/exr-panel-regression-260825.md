# The EXR milestone's panel regression, and the hand-test verdict

**2026-08-25, second session. Physical panel, 5120x1440 @ 239.999Hz. Branch
`exr-stage0-dependencies` at `00d6291`. NOT merged, no release cut.**

**NO PRODUCT CODE CHANGED IN THIS SESSION.** The only source edit is one
measurement harness (`seqreverse.ps1`) whose defaults had gone stale, plus
documentation. So there is no new shipping cost and none is claimed.

---

## THE OWNER'S VERDICT, WHICH IS WHAT THIS SESSION EXISTS TO RECORD

**~15 fps on the 27-channel multilayer DWAA working file is USABLE for review**
(owner, 2026-08-25), on the stated grounds that this is a difficult file class
that is often hard to play back in real time without caching. **That file is
explicitly not a blocker for the EXR milestone.**

Read at its stated width. What is accepted is **this file class, at this rate,
for review**. It is **not** a claim that the file reaches real time -- it misses
by about a third -- and the release notes are required to state both halves.
Wording: `docs/exr-release-notes.md`. Durable known-gaps entry:
`docs/release-notes-alpha.md`.

**The off-thread EXR read is therefore a future optimisation and not a blocker.**
Not started, by instruction.

---

## CHECK THE DISPLAY FIRST -- IT WAS THE RIGHT ONE THIS TIME

The brief warned the panel had been coming up at 59Hz. It was not:
`refresh.ps1` reports the active path as **5120x1440 @ 239999/1000 = 239.999 Hz**,
and `Win32_VideoController` puts the 4090 on 5120x1440. `parsecd` is running as a
daemon but **neither virtual adapter has an active mode**, so this is a local
session at the panel -- "Parsec off" and "no Parsec display is active" are
different claims and both were checked.

**Two independent corroborations came out of the runs themselves rather than the
probe**: the scrub HUD names the display (`scr Odyssey G95SC dpr 1.00`), and its
`stalls ... (>8.3ms)` threshold is 2x a 239.999Hz refresh. A 59Hz panel would
have printed `>33.3ms`.

---

## THE BINARY WAS IDENTIFIED BY ITS OWN STRINGS, NOT ITS TIMESTAMP

`Trace.exe` was dated one minute BEFORE the last source commit, which is
ambiguous. Settled by searching the binary for the marker that commit
introduced: HEAD carries `(%3% real time)` (the single-percent form from
`7369c29`) and `seq-prefetch`; `Trace_control.exe` carries `%% real time` and no
`seq-prefetch`. The two are provably distinct builds. Rebuilt at HEAD anyway --
exit 0, one pre-existing `C4834` warning in `MediaIoSource.cpp:75` from
`8692abb`, harmless (the next line tests `isOpen()`).

**`strings` IS BROKEN IN THIS GIT BASH** -- it returns **zero lines** from a
1.1MB PE file. The first search "found" nothing for every marker including ones
known to be present, which reads exactly like a build missing its features. Use
`grep -a` on the raw binary, and **prove the search finds a known marker before
believing any null result.**

---

## THE REGRESSION, FLAT

### Selftests and assets

| check | result |
|---|---|
| `--renderer-selftest=d3d11` | **`renderer=d3d11 fellback=0 planar=1`** (hardware path, not WARP) |
| `--renderer-selftest=cpu` | `renderer=cpu fellback=0 planar=0` |
| `--window-shape-selftest` | `OK - 11 shapes x 4 scale factors` |
| `--exr-channels-selftest` | `OK - 14 channel layouts` |
| `--ocio-selftest` | `version=2.5.2 ... rgb 0.18->0.34919 moved=1`, `differ=1` |
| `verify_trace_assets --strict --no-pillow` | `derived: 33 embedded files`, exit 0 |

**OCIO reads 2.5.2 off the running build**, which is the pin carrying the
CVE-2026-42450 `.cube`-parser fix. The pin did not move.

**The asset verifier was proven able to fail before its pass was accepted**: a
planted stray file in `assets/interface/transport/` gives exit 1 with
`FAIL unexpected file (nothing embeds it)`; removing it returns exit 0 and
`git status assets` is clean.

### Video -- unchanged

| leg | result |
|---|---|
| `scrubbar.ps1` full pool | **PASS -- 22 files, 88 legs, `delta 0` throughout** |
| 4K H.264 cadence x2 | **100.0% / 100.0%**, `drop 0`, `rephase 0`, `tick-late 0 of 119`, `tick-stall 0`, `handler>budget 0 of 119` (max 4.0/3.9), all 119 gaps `~1x` |
| ProRes 4444 cadence x2 | **99.8% / 99.8%**, 261 frames, `drop 0`, `rephase 0`, `handler>budget 0 of 260` (max 31.9/31.2), `thr slice x32` |
| 4444 `-SnapRelease` | **`target 261 shown 261 delta 0`**, `walk 0f`, **`dst YUV444P12 planar`** (GATE C intact), `release 21.0ms`, `hitch 0`, `land 0`, `kf-land 0` |

`delta` over all 88 scrub legs has **a single distinct value, `0`**. `kf_land`
is non-zero on **exactly two rows** -- Universe leg 2 (1) and WeLo leg 2 (21) --
the two recorded long-GOP rows, and **0 on the other 86 including every ProRes
row**. The decisive leg-2 rows all sit at or better than the recorded fix
figures:

| leg 2 | `behind_end` | `p2p_end` | recorded fix | `delta` |
|---|---|---|---|---|
| WeLo | 0 | **56.4ms** | 85ms | 0 |
| Universe | 0 | **30.9ms** | 57ms | 0 |
| Jeep (boundary file, bar 191ms) | 0 | **3.7ms** | 152ms | 0 |

### EXR / image-sequence playback

**DISCARD THE FIRST PASS.** Rep 1 is the OS file cache filling, exactly as
recorded.

**DWAA, 27ch, 97 frames, 3.3 GB** -- `seq-prefetch stride`, `issued 0 decl 59
legacy 4`:

| rep | presented | % real time | frames | skip | loads/frame |
|---|---|---|---|---|---|
| 1 (cold) | 14.30 | 59.6% | 59 | 38 | 1.02 |
| 2 | 15.31 | **63.8%** | 63 | 34 | 1.02 |
| 3 | 15.26 | **63.6%** | 62 | 34 | 1.03 |
| 4 | 15.63 | **65.1%** | 64 | 32 | 1.02 |
| 5 | 15.47 | **64.4%** | 63 | 33 | 1.03 |

**Warm: 15.3-15.6 fps, 63.6-65.1%** -- reproducing the recorded 64.4-65.5% and
the owner's ~15 fps. `media 97.7-98.5%`: the playhead holds the clock by
skipping, and `media` and `real time` are different questions.

**`loads 1.02-1.03/frame` is the figure that closes the scheduling question.**
The floor is 1.00 -- one read for the frame being shown and nothing speculative
-- so the shipped policy is within 3% of it and there is no speculative work
left to remove.

**PIZ, 3ch, 217 frames** -- the control file, 4 reps, **99.9% on every one**:
`skip 0`, `rephase 0`, `tick-late 0 of 215`, `tick-stall 0`, `handler>budget 0
of 215`, and `issued 212 decl 0 | cache hit 216 miss 0 (100.0%) | loads
1.00/frame`. A **perfect** hit rate, marginally better than the recorded 216/1.

**PNG, 168 frames** -- 3 reps, **100.0% on every one**: `skip 0`,
`handler>budget 0 of 166`, all 166 gaps `~1x`, `cache hit 167 miss 0`.

**The two EXR files behave oppositely and that IS the policy working**: PIZ
issues 212 predictions and declines none; DWAA issues none and declines 59.
Declining is the win on DWAA.

### The cpu escape hatch, on the EXR float path

PIZ on `TRACE_RENDERER=cpu`, five reps: **99.9, 96.6, 99.9, 99.9, 99.9%.** The
single dip did not reproduce across three subsequent reps and is inside the cpu
path's own recorded variance (a 94.6% outlier is on record for cpu 4444 and
likewise did not reproduce). Four of five at `skip 0` / `handler>budget 0 of
215`.

---

## EXACTNESS -- UNCHANGED, AND STRUCTURAL

The policy is gated on `playTimer_.isActive()`, so paused stepping and random
access take the legacy `+-1` window verbatim. Measured rather than assumed,
**shipping default against `TRACE_SEQ_PREFETCH_STRIDE=0`**:

| gesture | file | cross-config | negative control |
|---|---|---|---|
| step +7 then -7 | DWAA | **0% / 0%** | **10.4796%** |
| step +7 then -7 | PIZ | **0% / 0%** | **55.1105%** |
| reverse, stop, +3, -3 | PIZ | **0% / 0% / 0%** | **47.89%** |
| reverse, stop, +3, -3 | DWAA | **0% / 0% / 0%** | **8.7407%** |

The DWAA negative control reads **10.4796%**, the recorded figure to four
decimal places. **Every negative control fired**, so each 0% is a comparison
that could have detected a moved picture and did not.

### Reverse rate, as an A/B

| DWAA reverse | rollback (`legacy (env)`) | **shipped (`stride (env)`)** |
|---|---|---|
| presented | 6.63 (27.6%) | **14.63 (61.0%)** |
| frames | 27 | **60** |
| skip | 69 | **37** |
| `tick-stall` | 25 | **4** |
| loads/frame | 2.89 | **1.10** |

| PIZ reverse | rollback | **shipped** |
|---|---|---|
| presented | 17.68 (73.6%) | **23.97 (99.9%)** |
| skip | 56 | **0** |
| `handler>budget` | 57 of 159 | **0 of 215** |
| cache hit | 103/57 (64.4%) | **215/1 (99.5%)** |

**Each leg's configuration was read off its own HUD** (`seq-prefetch legacy
(env)` against `stride (env)`, and `dir -1` showing the signed counter
predicting backwards), never trusted from the command line.

**ONE DISCREPANCY, STATED AS ONE.** The record has PIZ reverse at
**84.2% -> 90.2%**; this session measures **73.6% -> 99.9%**. Both figures
differ from the record in **opposite** directions, so the measured gain is
larger here. Attributed to file-cache warmth -- four forward passes over that
1.6 GB sequence preceded it -- and `skip 0` with a 99.5% hit rate is a clean
warm result rather than a regression. Not smoothed over, and not proven either:
no cache-drop control was run.

---

## TWO HARNESS FAULTS, ONE NEWLY FOUND AND ONE SELF-INFLICTED

**`seqreverse.ps1`'s DEFAULTS WENT STALE THE MOMENT THEY WERE WRITTEN.** The
script was **created by `81f66b1`** -- the same commit that made the stride
policy the shipping default. `seqPrefetchStrideAware()` reads the knob as
"empty **OR** != 0", so an unset knob is **stride ON**. Its `EnvA` (profile
only, labelled `fixed`) and `EnvB` (profile + `STRIDE=1`, labelled `gate4`) are
therefore **the same configuration under two labels**, and a default run would
have compared stride against itself and reported the columns identical -- which
reads exactly like "the policy makes no difference".

The **recorded** reverse figures are safe: their `ISSUED/DECLINED` columns read
`--` against `110/81`, so that run passed explicit env. The trap was live for
the next person only. `EnvA` now names the rollback explicitly, and a guard
throws when both legs resolve to the same configuration.

**THE FIRST VERSION OF THAT GUARD WAS WRONG AND ITS OWN NEGATIVE CONTROL CAUGHT
IT.** It compared the knob's **text**, so absent-vs-`=1` looked different while
being the same configuration, and it let exactly the bad pair through. It
**resolves** the knob now, the way the C++ does. Proven to fire on all three
identical-config forms (`absent vs =1`, `=1 vs =1`, `=0 vs =0`) and proven not
to over-fire, because both real reverse runs executed. **Never rely on an unset
knob to mean a non-default: name the configuration you want.**

**A CONCURRENT RUN VOIDED THE FIRST FULL-POOL SWEEP.** A Trace-launching guard
test was run while `scrubbar.ps1` was sweeping; `scrubsweep.ps1` got **null
output** from a `--scrub-selftest` invocation and threw on `Split` at file
14/22. Re-run serialized, it passed 22/22. Note `seqcadence.ps1` ends with
`Stop-Process -Name Trace -Force`, so it kills any other Trace -- **measurement
runs on this project must be serialized, and a harness crash mid-sweep is worth
suspecting as contention before it is read as a defect.**

---

## WHAT WAS NOT DONE, BY INSTRUCTION

Off-thread EXR reads (now a future optimisation, not a blocker) - the warm-up
window - gate 8 - the GPU stage - Cryptomatte - merging to `main` - cutting a
release. The vcpkg pin did not move.
