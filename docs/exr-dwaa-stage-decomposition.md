# Where the DWAA sequence's ~190 ms frame actually goes

Record of what was measured. 2026-08-24, branch `exr-stage0-dependencies`,
physical panel **5120x1440 @ 239.999000 Hz**, shipping renderer
(`d3d11 +overlay`), `TRACE_HUD=1`, scratch settings file, root pass, no colour
transform.

**Nothing was optimised. The instrument is `TRACE_SEQ_PROFILE=1`, default off.**

---

## THE QUESTION

`15_Redshift_ACES_EXR\MultlayerAces` (97 frames, 1920x1080, 27 channels, DWAA)
presents at **~29% of real time** with a **~190 ms handler** against a 41.67 ms
budget, while `exrprobe --read` measures its selected channel span at
**~35 ms standalone**. That ~5x gap has been carried as unexplained since the
cadence instrument was built.

The pass-cycling leg narrowed it rather than closing it: passes whose standalone
reads differ by ~10 ms present identically, so **the missing time is invariant to
which channels are read.** A handler figure cannot say where its own time went,
so the stages were timed individually.

---

## THE ANSWER, IN ONE LINE

**It is `read_image` -- and it runs 2.79 times per presented frame.**

`prefetchNeighbors()` performs **two synchronous full loads on the UI thread,
inside the playback tick**, for frames at ±1. The playhead on this file advances
~3.4 frames per present (`skip 68-70` over 28-29 presents), so **the neighbours
it loads are almost never the frame presented next.** They are decoded, mapped
into the cache, and evicted unshown.

---

## THE TABLE

Warm run, profiler on, 28 presented frames, 78 loads:

| stage | per call | calls | **per frame** | share of accounted |
|---|---|---|---|---|
| **`read_image`** | 41.26 ms | 78 | **114.95 ms** | **79.4%** |
| `alpha prefill` | 4.61 ms | 78 | 12.85 ms | 8.9% |
| `map+convert` | 10.07 ms | 29 | 10.43 ms | 7.2% |
| `open+spec` | 1.77 ms | 78 | 4.93 ms | 3.4% |
| `group/choose` | 0.25 ms | 78 | 0.70 ms | 0.5% |
| `upload` | 0.64 ms | 29 | 0.67 ms | 0.5% |
| `loader tail` | 0.08 ms | 78 | 0.23 ms | 0.2% |
| `buffer alloc` | 0.01 ms | 78 | 0.04 ms | 0.03% |
| **ACCOUNTED** | | | **144.79 ms** | |
| *of which prefetch* | *87.30 ms* | *28* | *87.30 ms* | ***60.3%*** |

**THE ACCOUNTING CLOSES.** Handler p50 on the same runs is **147.2-150.0 ms**, so
144.79 ms accounted is **96.5-98.4%** of it. There is no material unexplained
residue left; what remains is `refreshHud`, tick bookkeeping and the repaint,
which is scheduled and runs outside the handler anyway.

**Read `calls` beside every total, always.** The loader rows run once per LOAD,
not once per presented frame -- **78 loads for 28 frames, 2.79 per frame**.
Dividing a loader total by the frame count without the call count reads a
three-load tick as one expensive load, which is exactly how ~35 ms of measured
read became a ~190 ms mystery.

### Per single load

`open 1.77 + group 0.25 + alloc 0.01 + alpha prefill 4.61 + read 41.26 +
tail 0.08` = **47.98 ms**. Times 2.79 loads = 133.9, plus `map 10.43` and
`upload 0.67` = **145.0 ms**, against the measured 144.79. The model closes to
0.15%.

---

## WHAT EACH NAMED SUSPECT TURNED OUT TO BE

- **EXR decode/read -- DOMINANT.** 41.26 ms per call, 79.4% of the frame. Note
  it is ~17% above `exrprobe`'s 35.35 ms standalone for the same span; the
  in-process figure is the one that counts.
- **Repeated per-frame setup / open / metadata -- REAL BUT SMALL.** Every frame
  re-opens the file and re-reads its header: `open+spec` 1.77 ms per load, 4.93
  ms per frame, **3.4%**. Worth knowing it is genuinely repeated; not worth
  removing on its own.
- **Mapping / normalisation + pixel conversion -- 10.43 ms per frame, 7.2%.**
  One call per presented frame, not per load, which is correct: only the frame
  being shown is mapped.
- **Buffer copies and frame construction -- effectively nil.** `buffer alloc`
  is **0.01 ms per load** (33.2 MB at 1080p RGBAF32) and `loader tail` --
  which carries the `QFileInfo` stat, the compression attribute and the struct
  fill -- is 0.08 ms. The float buffer being large is not costing anything.
- **Upload -- effectively nil, 0.64 ms per call, 0.5%.** See the cold-run trap
  below before quoting any other figure for it.
- **Presentation -- not in the handler.** The tick calls `update()`; the paint is
  scheduled and runs later in the event loop, so it cannot be part of the ~190 ms.
- **One stage that was not on the list and should have been: `alpha prefill`,
  12.85 ms per frame, 8.9% -- the second-largest term.** For a 3-channel pass
  `loadExr` writes opaque alpha across the whole float buffer before the read,
  which first-touches every page of a 33 MB allocation to set one component.

---

## WHAT THIS DOES AND DOES NOT PROMISE

**Removing the prefetch is the largest single lever available and it is not
enough on its own.** Arithmetic, stated as a projection rather than a
measurement, because it was not measured: at one load per frame the accounted
cost would be `47.98 + 10.43 + 0.67` = **59.1 ms per frame**, i.e. roughly
**29% -> ~41% of real time**. Still ~1.4x over a 41.67 ms budget, with
`read_image` at 41.26 ms then being 70% of a frame by itself.

So **there is no single fix here**, and the honest ranking for whoever picks
this up is:

1. **Stop prefetching frames the playhead will skip** -- 60.3% of the frame,
   and it is a scheduling question, not a decode question. Cheapest by far.
2. **The read itself** -- 41.26 ms, irreducible on this thread. Reaching 24 fps
   needs it off the UI thread or cheaper, which is architecture.
3. **The alpha prefill** -- 8.9%, and removable without touching decode.
4. Everything else is under 4% and not worth a change.

**The confirming experiment is not built**: a knob that disables
`prefetchNeighbors()` would turn the projection in (1) into a measurement, and
that is the recommended next step rather than anything in (2).

---

## TWO TRAPS THIS PASS PAID FOR

**A COLD PROFILE RUN IS NOT A PROFILE, AND IT PUT 163 ms ON THE WRONG STAGE.**
The first profiled run -- taken immediately after a rebuild, with the DWAA file
evicted from the OS cache by the preceding PIZ runs -- read **`upload` at 162.99
ms per call** and a handler max of **1929.7 ms** at 10.0% of real time. Warm, the
same binary reads `upload` **0.64 ms** and 28.9%. Had that first table been
reported, the dominant stage would have been named as the GPU upload -- the one
stage that is provably nil -- **and that is precisely the reading that would have
sent the next session into a GPU rewrite.** The mechanism by which a cold disk
stall landed inside the upload scope was not investigated; the operative rule is
that the first run after a build or after other media is not a measurement.

**THE INSTRUMENT WAS CONTROLLED BEFORE ITS OUTPUT WAS BELIEVED, AND THAT IS WHAT
CAUGHT IT.** Same binary, knob off, 3 reps: **29.0 / 29.4 / 28.7%** with handler
max 185.0 / 189.2 / 189.8 -- against the pre-instrumentation control's
28.6 / 29.1 / 29.3% and 186-190. Knob on, warm: **28.9 / 28.7%**, inside that
spread (one rep's handler max reads 206.4, above the off-spread; rep 1 reads
188.8, on it). Without that control the cold run would have looked like the
instrument costing 10x rather than like a cold file.

---

## PROVENANCE

Instrument `96ddab6`, `src/core/SeqProfile.{h,cpp}`, `TRACE_SEQ_PROFILE=1`,
default off, output `%TEMP%\trace_seqprofile.txt`. Reverts cleanly and the
reverted tree builds -- checked, not assumed. Selftests green on the
instrumented binary: `renderer=d3d11 fellback=0 planar=1`, `exr-channels OK -
14 channel layouts`.

**ACES performance is a separate track and is untouched here**: a `.cube` LUT is
free, ACES 1.0 costs ~+32 ms per frame, ACES 2.0 ~+92 ms
(`docs/exr-stage3-pass-cadence.md`). Every figure above is with **no transform
active**.
