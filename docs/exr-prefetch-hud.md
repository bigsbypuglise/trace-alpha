# The prefetch policy on the HUD, and the rep-to-rep spread explained

2026-08-25, branch `exr-stage0-dependencies`, physical panel **5120x1440 @
239.999000 Hz** (checked before measuring, per the standing rule -- the panel has
been coming up at 59 Hz).

Closes the gap `docs/exr-prefetch-shipped.md` named as *"the policy's state is
not on the HUD"*. **Nothing about the policy changed.** The commit adds
observation and corrects a stale comment.

---

## THE LINE

Fourth line of the image-sequence HUD, directly under the three cadence lines
because it is the explanation for them:

```
seq-prefetch stride | last-stride +1 | gate run 215/4 dir +1 warm 215/4
  | issued 212 decl 0 legacy 4 | cache hit 216 miss 0 (100.0%)
  | loads 215/216 = 1.00/frame
```

| field | what it answers |
|---|---|
| `seq-prefetch` | which policy: `stride` \| `stride (env)` \| `legacy (env)` \| `off (env)` |
| `last-stride` | how far the playhead moved between the last two PRESENTED frames. `+1` is a sequence holding its budget; `+2`/`+3` is the scheduler skipping, which is *why* a fixed `+-1` window decodes frames nobody sees. `--` is a discontinuity or nothing measured yet |
| `gate run N/4` | the predicate itself: CONSECUTIVE unit strides, reset to 0 by one skip |
| `warm N/4` | below 4 the warm-up is unfinished and the legacy window runs whatever the run says |
| `issued` / `decl` | what the gate DID |
| `legacy` | fixed `+-1` windows run: warm-up, paused stepping, and every frame under the rollback |
| `cache hit/miss` | the PRESENTED frame only |
| `loads` | REAL loader calls / presented frames -- the cost figure, comparable to the records' "loads per presented frame" |

**The policy field is tri-state on purpose.** The default and an explicit
`TRACE_SEQ_PREFETCH_STRIDE=1` behave identically and must not read identically,
or a figure taken under the rollback gets filed against the wrong policy. That
is the same reasoning as `strip`, `renderer` and `backdrop`.

**`loads` counts real loader calls, never decisions.** `prefetchFrameIntoCache()`
early-returns on an already-cached frame, so `issued x 1 + legacy x 2` is an
UPPER BOUND, and publishing that as "loads per frame" would overstate the cost
of the very policy the line exists to judge.

---

## THE THREE STATES, MEASURED

`MultlayerAces` DWAA 27ch unless stated. All warm (see below).

| | DWAA default | DWAA `TRACE_SEQ_PREFETCH_STRIDE=0` | PIZ default |
|---|---|---|---|
| policy field | `stride` | **`legacy (env)`** | `stride` |
| presented | **64.4-65.4%** | **29.1-30.1%** | **99.9%** |
| gate | `run 0-2/4` | `run 0-1/4` (unused) | **`run 215/4`** |
| issued / decl / legacy | `0 / 59-60 / 4` | **`0 / 0 / 29-30`** | **`212 / 0 / 4`** |
| cache hit | 3.1-4.7% | 6.9-13.3% | **100.0%** |
| **loads/frame** | **1.02-1.03** | **2.63-2.72** | **1.00** |
| `tick-stall` | 1-2 | **26** | **0** |

The line explains the cadence line above it in every column: DWAA declines
because its stride is not 1, and the win is 2.63 -> 1.02 loads per frame; PIZ
strides at exactly 1, so the gate opens fully and every presented frame is a
cache hit.

---

## COST: NONE, AND STRUCTURALLY ZERO WHERE IT SHIPS

Control built from `81f66b1`, the two binaries **proven distinct by their own
strings** (`seq-prefetch`, `last-stride` PRESENT in HEAD, ABSENT in control)
rather than by hash alone, both run warm, same clip, same duration:

| DWAA, 3 reps | rep 1 | rep 2 | rep 3 |
|---|---|---|---|
| control `81f66b1` | 65.5% (64f) | 64.5% (63f) | 65.5% (64f) |
| **HEAD** | 64.8% (64f) | 65.4% (64f) | 64.4% (63f) |

Same distribution, overlapping ranges, frames 63-64 on both.

**In the shipping configuration the cost is structurally zero, not merely
small**: `refreshHud()` returns above every line it builds when the HUD is
hidden, so none of this is constructed at all. What runs always is five integer
increments on a path that performs a synchronous EXR decode.

**Exactness unmoved**, which is the condition the policy was accepted under.
`seqstep.ps1`, default vs rollback: **0% differing at frame 7, 0% stepped back
to 0**, negative control **10.4796%**.

---

## THE REP-TO-REP SPREAD IS OS FILE CACHE

`docs/exr-stride-aware-prefetch.md` recorded a DWAA spread of **54.3 -> 60.6 ->
62.9%** as *"NOT explained ... it is the first thing to nail down"*, and ruled
out file caching on the grounds that the A and B columns were flat across their
own reps.

**Six consecutive passes this session, identical binary and config, say
otherwise.** Both EXR files rise monotonically to a plateau that matches the
recorded figure:

| pass | DWAA | PIZ |
|---|---|---|
| first ever this session (fully cold) | **37.6%** | -- |
| next 3 / next 2 | 56.9 -> 63.1 -> 63.7% | **76.5 -> 87.4%** |
| next 3 / next 2 | 65.5 / 64.5 / 65.5% | **99.9 / 99.9%** |
| next 3 | 64.8 / 65.4 / 64.4% | -- |

The sizes fit: **DWAA is 3.3 GB (33.8 MB/frame), PIZ 1.6 GB (7.4 MB/frame)**,
against **128 GB of RAM with 107 GB free** -- so both sets cache entirely after
a pass or two, and the first pass is disk-bound. Each rep is a fresh process, so
process warm-up cannot be it; thermal drift would push the other way.

The earlier ruling-out is consistent rather than wrong: those A and B columns
were measured later in an already-warm session.

**Practical consequence, and it applies to every EXR figure taken here: DISCARD
THE FIRST PASS OVER A SEQUENCE.** A cold first pass reads roughly half the rate
of the warm plateau, and on the fully-cold run it read **37.6% against 65%** --
which would look exactly like a serious regression.

Stated at its width: no deliberate cache-drop control was run, so this is a
strongly supported attribution across two independent files, not a proof.

---

## THE HAND TEST

Launchers in `_handtest_260825/` (untracked -- they carry absolute paths), full
procedure in `_handtest_260825/HAND-TEST.txt`. Recorded here so the procedure
survives the folder.

**Two rules first, or the result misleads.** MAXIMISE the window -- at the
default size the new line is clipped by ~10px. And PLAY EVERYTHING TWICE,
judging the second pass, because a cold first pass reads roughly half the warm
rate.

| test | what to do | expect |
|---|---|---|
| **1. headline** | DWAA, default then rollback, second pass each | **~15 fps (64-65%)** against **~7 fps (29-30%)**; `loads ~1.02` against `~2.7` |
| **2. control** | PIZ, default then rollback | **99.9% both** -- the policy must not hurt the easy file |
| **3. reverse** | PIZ, End then J | default **~90%**, rollback **~84%** |
| **4. exactness** | paused, Right x5 then Left x5 | same frame AND same picture; `legacy` climbs, `issued` does not |

**Wrong:** default stuttering more than rollback on any file; PIZ worse on
default; stepping not returning; the HUD saying `stride` under a rollback
launcher; `issued` climbing past a handful on DWAA; `loads/frame` above ~1.1 on
default.

**Not wrong, by design:** the two files behave oppositely -- PIZ issues, DWAA
declines, and declining IS the win there. And **DWAA at ~65% looks like ~15 fps
and is not meant to be real time yet**: a full load on that file is ~42.5ms
against a 41.67ms budget, so the read alone is the whole budget.

---

## STILL OPEN

- ~~**No owner hand-test.**~~ **DONE 2026-08-25, AND THE VERDICT IS USABLE.**
  The owner ran the procedure below and ruled that **~15 fps on the 27-channel
  multilayer DWAA working file is usable for review**, on the stated grounds
  that this is a difficult file class that is often hard to play back in real
  time without caching. **It is explicitly NOT a blocker for the EXR
  milestone.** Read that at its stated width: what was accepted is *this file
  class at this rate for review*. It is **not** a statement that the file plays
  in real time -- it does not, by about a third -- and the release notes are
  required to say both halves (`docs/exr-release-notes.md`).
- The warm-up window still uses the legacy `+-1` behaviour, worth the recorded
  1-2 points on DWAA. Deliberately not optimised.
- ~~Cosmetic, PRE-EXISTING and not this commit's: several HUD fields print a
  doubled percent (`56.9%%`, `media 98.5%%`, `wasted 98%%`) because
  `QString::arg` does not treat `%%` as an escape.~~ **FIXED in `7369c29`, the
  commit immediately after this record was written** -- every affected format
  string now carries a single `%`. Verified at HEAD: the only `%%` left in
  `MainWindow.cpp` and `src/core/*.cpp` are inside comments explaining the trap.
  The new line still folds the sign into the value, which is why it never had
  the bug.
