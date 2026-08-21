# Fullscreen aliasing — investigation record (2026-08-20)

**DECIDED AND BUILT (owner, 2026-08-21): option 2, narrowed to fullscreen.** The fullscreen
fit filters its magnification (linear); deliberate zoom — Actual Size, Zoom In, fullscreen or
not — keeps phase 15's nearest. `49ed100`; `TRACE_FS_MAG_FILTER=0` is the rollback. Measured:
720p fullscreen reads `filtered x1` on both backends where it read `NEAREST`, the knob
restores `NEAREST`, and fullscreen + Zoom In reads `NEAREST zoom 2.00:1` — the deliberate-zoom
half intact. Read at its stated width: the WINDOWED fit still takes nearest when it magnifies
(a source smaller than a hand-enlarged window), which for ordinary media requires deliberately
growing the window past the source's size — recorded as the decision's edge, not an oversight.
The rest of this document is the investigation as it stood before the decision.

Tester report: the picture looks aliased / over-sharpened in fullscreen. Owner has not
reproduced it. **Investigation only — nothing was built, per instruction.** The answer is a
product decision for the owner, with options at the bottom.

## The finding, in one sentence

**Confirmed: it is spec phase 15's magnification filter, working as specified.** Fullscreen
magnifies whenever the source is smaller than the screen, and above 1:1 both backends draw
with NEAREST — the owner decision of 2026-08-11 ("a review tool at 4:1 shows pixels") — so a
1080p or 720p file in fullscreen on a large panel shows stair-stepped edges. It is not a
defect and not a regression; it is the phase 15 decision reaching a case it was not chosen
for.

## Measurements (2026-08-20, physical panel 5120x1440 @ 239.999Hz, build at `860216c`)

**State proof, read off the HUD** — 720p pool file (`14_720P_Comfyui_mp4`), fullscreen,
HUD shown:

| config | HUD reads |
|---|---|
| d3d11, default | `display 1912x1076 NEAREST \| win 5120x1440 \| renderer d3d11 +overlay` |
| d3d11, `TRACE_MAG_FILTER=linear` | `display 1912x1076 filtered x1` |
| cpu, default | `display 1912x1076 NEAREST \| renderer cpu +overlay` |

So the **fit path** takes the point sampler above 1:1, not just deliberate zoom — confirmed
in code too: both backends gate on `reducing || magnifyLinear`
(`D3D11VideoRenderer.cpp:1148`, `CpuImageRenderer.cpp:132`), and the fit is "not reducing"
whenever the source is smaller than the viewport.

**Visual A/B, shipping configuration** (HUD hidden via `H` after F11, so the viewer takes
the full screen) — 1080p pool file, frame 100, fullscreen = 2560x1440 drawn size, a 1.333x
magnification:

- d3d11 nearest vs linear: **5093 of 63724 sampled px differ (>2), max channel delta 61**
- cpu nearest vs linear: **4957 of 63724, max delta 62** — same class, so the two backends
  agree and this is not a backend bug
- The crops show it plainly: hard edges (character outlines, speculars) stair-step under
  nearest and are smooth under linear. At the **fractional** 1.333x ratio the duplication is
  uneven — every third source pixel becomes two device pixels — which in motion reads as
  shimmer/"over-sharpening", matching the tester wording.

## Why the owner has not seen it — the arithmetic

Fullscreen scale = screen height / source height (all these fits are height-bound):

| source | owner's 5120x1440 | a 4K UHD panel (2160p) |
|---|---|---|
| 4K (2160p) | **0.67x — reduction, box-filtered** | 1.0x |
| 1080p | 1.33x nearest | **2.0x nearest** |
| 720p | 2.0x nearest | 3.0x nearest |

The owner tests mostly 4K material, which his panel *reduces* in fullscreen — step 9's box
filter, signed off. And one measurement subtlety that cost this investigation a round: **with
the dev HUD shown, 1080p fullscreen on this panel fits at 1912x1076 — fractionally UNDER
1:1 — and reads `filtered`**. The HUD's height is exactly what kept the magnification from
engaging. So even a deliberate check with the HUD up would not have shown it; only the
shipping (HUD-hidden) fullscreen does. Windowed is immune for ordinary sources: §4's opening
cap is a 1280x720-equivalent area, so anything 720p and up opens at or below 1:1.

## The options (owner call — do not change the filter without it)

1. **Keep nearest everywhere** (today's behaviour). No work. Fullscreen of
   smaller-than-screen sources keeps reading aliased to testers on large panels; the answer
   to the report is "working as designed".
2. **Bilinear when magnification was not explicitly requested.** The fit path (fullscreen,
   Fit to Window) filters when it magnifies; Actual Size and Zoom In keep nearest, so the
   phase 15 rationale — deliberate zoom inspects samples — is preserved where it was argued.
   Mechanically small: the D3D11 backend already owns both sampler states and the CPU path
   both hints; what is new is the renderer being told whether the current scale is a fit or a
   requested zoom (one flag through the `VideoRenderer` boundary, same shape as the view
   transform). Bilinear at 1.3-3x magnification is well inside that filter's quality range
   (its weakness is >2x *reduction*, which stays on step 9's box filter). Cost expectation is
   nil on d3d11 (a sampler state swap) and nothing new on cpu (the hint already exists);
   measure anyway.
3. **A user setting.** Most surface: a menu row, a persistence question (see Loop's
   deliberate non-persistence), and it reopens the phase 15 sign-off as a preference rather
   than a decision. Only worth it if the owner wants both behaviours reachable.

Note if option 2 is taken: phase 15's point-sampled **chroma** reasoning ("at 4:1 an 8x8
block of one colour is the honest reading") stays true for deliberate zoom and stops
applying to the fit case, which is consistent — the fit case is exactly where nobody asked
to inspect samples.

Harness: `scratchpad fsalias.ps1 / fsalias2.ps1` (session-local); captures were taken with
`restart.ps1` + `capture.ps1`, frame landed via Ctrl+G for cross-config comparability.
