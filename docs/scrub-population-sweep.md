# The scrub population sweep — part 1 of the reliability phase

2026-08-20, dev box. The characterisation `docs/scrub-reliability-phase.md` part 1 asks for:
`--scrub-selftest` over every H.264 MP4 and every ProRes in `Trace_Testing_Assets`, three
display passes, one table, the reading. **No fix is proposed in this document — that is part
2's work, after the owner has read this.**

## Method

- **Build:** `ebc1fa7` (the ×1.05 gate margin, HEAD). The binary predates the commit by six
  minutes; an incremental rebuild was run and relinked nothing, which is the proof it matches
  the source at HEAD.
- **Machine:** AMD Ryzen 9 5900XT, 32 logical / 16 cores, Windows 11 25H2, Odyssey G95SC
  5120x1440 physical panel, dpr 1.00. Renderer `d3d11 +overlay` on every leg (read off each
  report, not assumed).
- **Three passes, 22 files × 4 legs = 264 legs total:**
  1. **240Hz** — the panel's native 239.999Hz, the historical baseline.
  2. **60Hz** — via `setrefresh.ps1` (restored to 240 afterwards), what most users have.
  3. **60Hz + `TRACE_PRESENT_SYNC=1`** — the blocking-present class. Run **at 60Hz** because
     that is the recorded fault model of the Threadripper class; note the model is far
     harsher on this box than the real machine it models (paints block ~250ms here against
     ~16.7ms there — recorded 2026-08-19).
- **Run plain:** HUD hidden (each report's knobs line reads `hud hidden`), scratch
  `TRACE_SETTINGS_FILE` deleted per run, every other `TRACE_*` removed from the environment
  first (each report's env line is the check), cursor parked at (10,10) so the chrome is not
  held revealed. The selftest itself resizes to a fixed 1280x760 logical window per leg and
  reopens the media per leg, so every leg is a cold, cleanly-counted sample.
- **Harness:** `scripts/measure/scrubsweep.ps1` (runs + parses to CSV),
  `scripts/measure/gopprobe.ps1` (the ffprobe half). Raw reports and CSVs from this session
  are in the session scratchpad; the scripts regenerate them from scratch in ~20 min/pass.
- The Seedance HEVC file is included as a **reference row** — it is outside the stated
  population (HEVC) but is one of the three fixed faults, so the sweep doubles as the check
  that it stays fixed.

**Every one of the 264 legs is a structural PASS: `delta 0` on every release, on every file,
at every display pass — including the worst rows below.** The exactness contract holds across
the whole population. Everything that follows is about the *drag preview trailing the
pointer*, which is feel, not correctness.

## The population (ffprobe, decode-order packet scan)

| file | codec / profile | pix_fmt | res | fps | frames | keyframes | GOP gaps | audio |
|---|---|---|---|---|---|---|---|---|
| Universe_rc07_I_9x16_Online.mp4 | h264 Main | yuv420p | 1080x1920 | 23.976 | 412 | **9** | 28x1 48x8 | aac |
| M&M_TopGun_1080.mp4 | h264 High | yuv420p | 1920x1080 | 24 | 241 | 9 | 7,26,28,30x6 | aac |
| Marinelaverse_16x9_EndTag_v007.mp4 | h264 Main | yuv420p | 1920x1080 | 24 | 97 | 4 | 7,30x3 | none |
| Splash_1.mp4 | h264 Main | yuv420p | 3840x2160 | 24 | 121 | 5 | 1,30x4 | aac |
| Jeep_Snowboard_Drone_60FPS.mp4 | h264 High | yuv420p | 3840x2160 | **60** | 163 | 6 | 13,30x5 | none |
| video_ComfyUI_0000Fly8.mp4 (720p) | h264 High | yuv420p | 1280x720 | 24 | 361 | 7 | 6..98 irregular | none |
| _model_veo31_202512171427_8auub.mp4 | h264 High | yuv420p | 720x1280 | 24 | 192 | **1** | single GOP | aac |
| A_slow_controlled_202512171507_fbb81.mp4 | h264 High | yuv420p | 720x1280 | 24 | 192 | **1** | single GOP | aac |
| Best_02.mp4 | h264 High | yuv420p | 1280x720 | 24 | 144 | **1** | single GOP | aac |
| Dram_Theater_8_endcard.mp4 | h264 Main | yuv420p | 1920x1080 | 23.976 | 96 | 2 | 48x2 | aac |
| WeLo-AI-rc13-1x1_SOCIAL.mp4 | h264 Main | yuv420p | 1080x1080 | 23.976 | **720** | 44 | min 3 med 19 max 23 | aac |
| anamorphic-2x1-to-239.mp4 (fixture) | h264 High | yuv420p | 1920x816 | 24 | 144 | **1** | single GOP | none |
| anamorphic-4x3-to-16x9.mp4 (fixture) | h264 High | yuv420p | 1440x1080 | 24 | 144 | **1** | single GOP | none |
| rotated-90.mp4 (fixture) | h264 High | yuv420p | 1920x1080 | 24 | 144 | **1** | single GOP | none |
| rotated-180.mp4 (fixture) | h264 High | yuv420p | 1920x1080 | 24 | 144 | **1** | single GOP | none |
| TheraTears_Vial_VFX_v002.mov | prores 4444 | yuva444p12le | 4096x2304 | 24 | 262 | all | intra | none |
| Barritas_16x9_Shot_040-080_v005_ALT_1.mov | prores HQ | yuv422p10le | 3840x2160 | 24 | 169 | all | intra | pcm |
| FY27 1x1 (Lago).mov | prores HQ | yuv422p10le | 1080x1080 | 23.976 | 528 | all | intra | pcm |
| FY27 4x5 (Pool).mov | prores HQ | yuv422p10le | 1080x1350 | 23.976 | 528 | all | intra | pcm |
| Foces_8K Dino Stomp plate.mov | prores XQ | yuva444p12le | 7680x4320 | 23.976 | 145 | all | intra | none |
| V1-0004 (4448x3096).mov | prores 4444 | yuv444p12le | 4448x3096 | 23.976 | 108 | all | intra | none |
| video_ComfyUI_00004_.mp4 (Seedance, ref) | **hevc** Main 10 | yuv420p10le | 2160x3840 | 24 | 97 | **1** | single GOP | none |

Two population facts worth stating on their own. **Five of the fifteen H.264 MP4s are
single-GOP** — one keyframe for the whole file — and all five are AI-tool exports or
fixtures, the same structure as the fixed Seedance HEVC fault. This is a format class the
tools Anj's world uses actually produce. And **the pool's longest H.264 file (WeLo, 720
frames) has its shortest GOPs (median 19)** — GOP length and file length are independent
axes, and the sweep's worst row is short-GOP.

## Leg 2 — the reversal drag, the gesture that separates the population

Whole-clip traversals with hard direction changes. Demand (`ptr f/s`) is the gesture's frame
count over its fixed wall time, so it **scales with the clip's frame count**; supply
(`dec f/s`) is what the decode chain sustained. 240Hz figures; the 60Hz column is the same
metric at pass 2. `behind` and `p2p` **end** (at release, after a 400ms settle) are the
discriminators — `p2p max` reads ~1s even on healthy files (the recorded reversal-gesture
artifact at the sweep ends) and is not one.

| file | ptr f/s | dec f/s | supply | behind end/max | p2p end ms (240 / 60) | hitch (240/60) | stride |
|---|---|---|---|---|---|---|---|
| **WeLo 1x1 720f** | **978** | **520** | **53%** | **216 / 425** | **1694 / 700** | 18 / 16 | 1 |
| **Universe 9x16 412f** | **560** | **433** | **77%** | **0 / 176** | **248 / 269** | 9 / 9 | 1 |
| **Jeep 4K60** | 221 | 181 | 82% | 0 / 74 | 204 / 209 | 4 / 4 | 1 |
| Seedance HEVC (ref) | 131 | 125 | 96% | 0 / 54 | 0 / 0 | 10 / 7 | 1 |
| Splash_1 4K | 163 | 174 | 106% | 0 / 30 | 14 / 13 | 2 / 2 | 1 |
| M&M 1080p | 326 | 361 | 111% | 0 / 52 | 0 / 0 | 9 / 9 | 1 |
| 720p ComfyUI | 490 | 564 | 115% | 0 / 33 | 8 / 0 | 1 / 1 | 1 |
| veo31 / A_slow / Best_02 / Dram / fixtures / Marinelaverse | 129–260 | 150–299 | 115–116% | 0 / ≤9 | ≤8 | 0 | 1 |
| TheraTears 4444 | 355 | 61 | **17%** | **0** / 81 | **4.5** / 5.9 | 2 / 1 | **5** |
| Barritas 422 HQ | 229 | 151 | 66% | 0 / 15 | 11 / 7 | 1 / 0 | 1 |
| FY27 1x1 ProRes | **718** | 209 | **29%** | **0** / 17 | **2.0** / 0.6 | 0 / 0 | **2** |
| FY27 4x5 ProRes | **718** | 206 | **29%** | **0** / 22 | **4.5** / 1.7 | 0 / 0 | **3** |
| 8K 4444 XQ | 195 | 18 | 9% | 0 / 84 | 77 / 66 | **42 / 26** | 1 |
| 4448x3096 4444 | 146 | 46 | 32% | 0 / 26 | 0.4 / 4.4 | 4 / 4 | 1 |

The WeLo row verbatim, because it is the pool's worst and every subsystem in it is healthy:

```
pointer 978.1 f/s | presented 1508 f | dec 520.0 f/s | supply 53.2% | behind 216/425f (end/max) | p2p 1693.5/2727.1ms (end/max)
round trip/request 5.31/106.7ms | overhead 0.04/0.3 (req 476) | overhead share 0.7% of round trip
batch cap 4 achieved-avg 3.18 max 4 | budget-cut 42 of 477 | stride 1
seeks 24 | ra-walk 8.95f/seek | walk max 22f | rev-hit 96.3% | cache 224/224 (383.9/384.0 MB)
ui gap 1.01/2.5ms | release 37.3ms | target 0 shown 0 delta 0 -> leg PASS
```

The worker decoded 1,514 frames at 1.66ms each — 520 f/s sustained — with 0.7% overhead,
96.3% cache hits, a full-at-budget cache, batching near its cap and a 2.5ms worst UI gap.
Nothing in the fixed machinery is misbehaving. The pointer asked for 978 f/s, the never-skip
rule queued the difference, and the picture ended 1.7 seconds behind the hand.

## Legs 1, 3 and 4 in brief

- **Leg 1 (whole clip forward in 1.5s, snap release):** every H.264 file reads supply ~100%,
  `behind end 0`, `p2p end` under 5ms, at 240Hz **and** 60Hz. ProRes: sampling holds the
  1x1/4x5 at the pointer at 51% supply; TheraTears trails slightly (`behind end 3`,
  `p2p 19/79ms`); the 8K plate is the known decode-bound case (`p2p 60/249ms`, hitch 24).
- **Leg 3 (~3x, the latency leg):** clean everywhere; **worker overhead is 0.02–0.04ms per
  request on every file** — round trip ≈ decode, so there is no fixed-cost per-request term
  on this machine, exactly as the Threadripper diagnosis predicted for a healthy box. The
  Seedance release of ~707ms is the recorded single-GOP landing walk (the owner-ruled
  contract case, unchanged).
- **Leg 4 (rapid back-and-forth, the owner's gesture):** the same three files stand out and
  everything else is flat — Universe `behind max 42, p2p max 1227ms`; WeLo `39 / 926ms`; M&M
  `10 / 219ms`; all recover by each throw's end (`behind end 0`).

## The three passes against each other

- **60Hz ≡ 240Hz.** Across all 88 file-legs the two passes are the same within run-to-run
  variance — supply, behind, p2p, hitch, all of it. **The 239.999Hz panel is not currently
  hiding a systemic scrub weakness on this machine class**, because the scrub paint gate
  (`b830027` + `ebc1fa7`) made the refresh rate a non-variable: the gate reads 4.38ms at
  240Hz and 17.50ms at 60Hz (refresh × 1.05) and paints below the drain rate at both. This
  is a *post-gate* statement — the gate is three days old, and this sweep is the first
  population-wide evidence it generalises.
- **60Hz + `TRACE_PRESENT_SYNC=1` is contained but noisy.** Paint-cost EMA reads 70–265ms —
  this box's recorded harsh rendition of the blocking class (~250ms per blocked paint against
  the Threadripper's ~16.7ms). Under it, hitch reads a uniform ~11 on leg 2 across the whole
  pool and `p2p end` stays ≤10ms on every healthy file; `behind max` inflates by ~30–50
  frames everywhere; releases pay ~250ms when a landing paint blocks. **Per-file differences
  inside this pass are noise, not classes** — structurally identical fixture files split
  (anamorphic-4x3 read `behind end 26` while anamorphic-2x1 read 0, rotated-90 read 18 while
  rotated-180 read 0), which is what stochastic 250ms blocks landing at different moments
  look like. Do not rank files on the sync pass.

## The reading

**1. Which files and conditions are bad.** Three H.264 files, on *every* display pass, on one
gesture class: **WeLo (severe — ends 216 frames / 1.7s behind), Universe (the reported file —
peaks 176 frames / 2.7s behind mid-gesture, ends ~250ms behind), Jeep 4K60 (mild — ends
~205ms behind)**, under reversal and rapid gestures. Plus the two known heavy-plate cases (8K
XQ decode-bound everywhere; TheraTears trailing slightly on leg 1). Everything else — eleven
H.264 files including every single-GOP AI export, and four of six ProRes — is clean on every
leg of every pass.

**2. What the bad share that the good do not.** All three are **long-GOP H.264 under a
gesture whose pointer demand exceeds sustained decode supply** — dec/ptr 0.53, 0.77, 0.82,
against ≥0.96 for every clean H.264 file. The boundary sits at supply ≈ demand, and severity
is monotone in the ratio. Demand scales with clip frame count (a whole-timeline gesture
covers more frames on a longer clip in the same wall time), supply with per-frame decode
cost; WeLo fails on length (720 frames at 1.66ms), Jeep on cost (163 frames at 3.69ms at
4K60), Universe on both (412 frames at 1.95ms). On long-GOP the never-skip rule then turns
the deficit into accumulated lag — the picture walks every intermediate frame, seconds behind
the hand.

**3. The same deficit on ProRes does not produce the fault, and that contrast is the
sharpest fact in the table.** The 1x1/4x5 ProRes rows face a *worse* ratio (0.29 — 718 f/s
demanded, ~207 supplied) and end **at the pointer** (`behind end 0`, `p2p end` 2–5ms),
because §15's velocity-adaptive sampling strides them (stride 2–5). TheraTears at ratio 0.17
likewise ends at the pointer. **The separator between the failing set and the healthy set is
not a tuned constant — it is the sampling gate (`AV_CODEC_PROP_INTRA_ONLY`).** The bad files
are exactly those where sampling is off and demand exceeds supply.

**4. The standing hypothesis (five machine-tuned constants) is largely refuted by the
table.** The 60Hz pass equalling 240Hz refutes display-dependence of the time-budget
constants on this machine class. On the failing files the constants are not the binding
term: batch achieves 3.1–3.2 of cap 4 with round-trip overhead at **0.7%** (amortisation is
done — a bigger cap buys ~nothing), walk-budget cuts touch 16–42 of ~400+ requests while the
ceiling is decode f/s, and coalesce/ease behave identically at both refresh rates. What
*survives* of the hypothesis is its contrast clause, now sharpened: the two adaptive
mechanisms (sampling, paint gate) again produced zero reports, and the failure lives
precisely where the adaptive mechanism is gated off.

**5. A caution for part 2, from this project's own record.** "Open the sampling gate for
long-GOP" is not a conclusion this table licenses on its own: §15 measured naive long-GOP
striding as catastrophic (hits 85.4% → 13.3%, decode 90 → 14 f/s — a strided step leaves the
walked GOP and pays seek + walk where adjacent steps were cache hits), and four inferred
gates failed before the codec-property gate shipped. Any part-2 design has to reconcile the
class boundary this table shows with that recorded mechanism — presumably why the phase
separates characterisation from fix.

**6. The fixed faults stay fixed.** Seedance HEVC: leg 2 `p2p end 0`, release 181–190ms, leg
1 landing ~707–728ms (the ruled contract case). 720p ComfyUI: clean on every leg (batch
achieved 1.75, `p2p end` ≤8ms). M&M at 240Hz reads its recorded class to the digit (leg 2
rev-hit 97.8%, walk max 29, hitch 9); its Threadripper fault was the present path, modelled
here by the sync pass and contained by the gate. The exact release (`delta 0`) held 264 of
264 times.

**7. What the fourth report is, in the table's terms.** `Universe_rc07_I_9x16_Online` "and
others" on the dev box is the demand-over-supply long-GOP class — and the sweep found a
worse member the report did not name (WeLo) plus a milder one (Jeep). It is not the
Threadripper's present-blocking (this box does not block; pass 3 models it and the gate
contains it), not the 720p round-trip fault (overhead 0.7%), not the Seedance landing walk
(seeks 10–24, ra-walk 9–35f, all healthy). It is a fourth distinct mechanism, as the phase
brief predicted — **the one the design chose deliberately**: never skip a frame during a
long-GOP drag, so when the hand outruns the decoder, the picture trails by the whole
deficit.

## Harness facts from this session

- **PowerShell `-match` is case-insensitive, and the first GOP probe reported every frame of
  every file as a keyframe** — matching packet flags with `-match 'K'` matches the lowercase
  k in the word "packet" on every csv line. On this pool that result is *plausible* for
  ProRes and catastrophic for the reading. `-cmatch ',K'` is the fix; the tell was keyframes
  == packets on files ffprobe had just called long-GOP.
- **`Start-Process -PassThru` without touching `.Handle` reads an EMPTY `ExitCode` under
  Windows PowerShell 5.1** even after `WaitForExit(ms)` returns true — cache `$null =
  $p.Handle` immediately after launch. (The recorded `Start-Process -Wait` rule covers this;
  the waitless variant needs the handle cached explicitly.)
- The sweep runs unattended and needs no foreground: the selftest drives the real slider
  programmatically, so nothing here depends on `SetForegroundWindow` or `SendKeys` — which is
  why 66 runs could go by without a single harness flake.
- `setrefresh.ps1`'s change is dynamic; this session restored 240Hz explicitly and verified
  it. Any future pass must do the same or every subsequent measurement on the box inherits
  60Hz silently.
