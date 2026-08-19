# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Trace is

A fast, minimal Windows desktop media player for professional review workflows (editors, VFX artists, motion designers, AI video creators). Three pillars: **Simple** (clean black stage, no libraries/playlists), **Fast** (instant launch/load, responsive scrub), **Trustworthy** (frame order, stepping, and timing must be exact — "next frame" means the actual next frame). It sits between editing/compositing apps: open a render, check a frame, move on. Not an editor, not an asset manager.

Current alpha focus: 4K H.264 MP4 + ProRes MOV playback, reliable reverse playback, frame-accurate stepping, trustworthy scrubbing. Formats and UI features come after the playback foundation is dependable. Longer-term: image sequences, EXR, OCIO color management, timecode/frame HUD (partially present).

**Owner priority order (2026-08-09), which outranks any roadmap item:** performance is #1 —
no interface feature may ever compromise lightweight, fast, smooth playback; if a feature and
playback smoothness conflict, the feature loses. Interface work is explicitly paused. The goal
for the current phase is the core playback experience alone: smooth playback, locked real-time
playback, responsive polished scrubbing at slow and fast speeds in both directions, and strong
GPU integration. Everything else comes after that foundation is working extremely well.

**THAT PHASE IS ACCEPTED AS COMPLETE (owner, 2026-08-10)**, for four things stated
deliberately narrowly: **smooth *forward* playback; exact real-time scheduling; responsive
*bidirectional scrubbing*; and the *SDR* D3D11 GPU integration.** Read each at its stated
width and do not let a later summary widen it — it is *forward* playback, not playback in
general, because **continuous reverse is explicitly the next item and was not accepted**;
and it is the *SDR* integration, because 10-bit output and HDR are out.

**Step 10, 10-bit display output, is FORMALLY DEFERRED with two external gates** (owner,
2026-08-10): it is *not* a playback-performance or GPU-integration blocker for the current
SDR base version, and it is not to be built until (a) a 10-bit-capable output display is
confirmed and (b) the intended Windows Advanced Color / HDR / colour-management workflow is
defined. Both gates are outside the code. §9's warning still applies — do not conflate it
with the high-bit-depth *processing* that shipped at GATE C.

**THE SHUTTLE PHASE IS ACCEPTED AS COMPLETE (owner retest, 2026-08-10).** Fast-forward
advances clearly through the whole ladder on every format; reverse 30x reads as intentional
at the approved ~15fps presentation cadence; direction changes respond correctly; stopping
lands on the last visibly displayed frame; and normal playback, audio return, scrubbing,
exact release and stepping all remain good. Every goal in the phase brief below is met and
verified — the brief is retained as the record of what was asked for, not as open work.
**The subjective half was taken at the machine, not over Parsec.**

Read the scope at its stated width, as with the playback phase before it: what is accepted
is the **engine**. The 2x/5x/10x/30x **interface** remains deferred and unstarted, and
carries one requirement that is easy to lose — **the buttons must begin at 2x on their
first click**, while the J/L keyboard convention keeps 1x as its first rung. The owner
confirmed both readings; a button that inherits the keyboard ladder wholesale is wrong.
`startShuttleRun(direction, stride)` takes any stride, so that is a call site rather than
engine work.

**The phase brief, retained as the record of what was asked for:** (owner, 2026-08-10). The planned
interface includes 2x, 5x, 10x and 30x rewind, and reverse at 1x measures 86.7% of real time
on 4K H.264 (plan §29.3), so exposing rewind controls now would surface a known weakness.
Note this is an interface spec driving an *engine* requirement — starting it is not a breach
of the no-interface-work rule. **At accelerated reverse speeds, every source frame is NOT
required.** The goals: immediate response when rewind is pressed; stable, intentional visual
cadence; newest-target-wins; no UI-thread saturation; rapid direction changes; appropriate
sampling at each speed; **exact frame landing when rewind stops**; and **no regression to
forward playback, scrubbing, stepping or audio state.** The last two are the invariants, and
they are where every previous reverse or scrub attempt in this project actually failed.
**Begin with measurement and an architecture proposal, before implementation.** Reuse the
validated async scrub/cache infrastructure where appropriate, but **do not weaken exact
scrub release or increase normal playback cost.** Full brief in `docs/next-session-prompt.md`.

**THE SHUTTLE IS BUILT AND MEASURED (2026-08-10, `e9fd236`; plan §11a).** Reverse decode
runs on the existing scrub worker under the existing lease, results are **queued** and the
tick pops one per slot, and **the stride is the commanded speed** while presentation stays
at one frame per source period. 4K H.264 reverse: **1x 87.0 → 99.2% of real time** with
`handler>budget 11 → 0` and worst handler **132.6 → 6.3ms**; **2x 75.7 → 100.1%**; 5x 95%;
10x ~9.8x; 30x ~26x. ProRes 4444 1x **99.7 → 100.0%** (handler 24.46 → 3.87ms) and **10x in
full at 24 presents/s, `starve 0`**. `TRACE_REVERSE_ASYNC=0` is the control. **Still open:
the keyframe snap for high speeds on files whose GOP does not divide the stride — 1080p
reaches ~20x of 30x — and an owner question about which way that trade should go.**

**The measurement pass and the proposal are in `docs/reverse-shuttle-plan.md`.** Read that
document before proposing anything here. Three results decide the shape of the work and each is measured:
**(a) at reverse 1x the decoder is IDLE 80–93% of the time** on long-GOP and still misses
real time — the deficit is burstiness, not throughput, which is the opposite of the drag
path and is why §15.3's decline of directional prefetch **does not carry over**;
**(b) ProRes reverse at 1x is already perfect** (4444 reads 99.7% of real time, zero
handlers over budget) because a seek lands on the target — the intuition that 4444 is the
hard file is inverted here; **(c) a keyframe-aligned reverse sample costs ~30ms and a
walked frame costs 1.7–2.6ms**, which is what makes a *snapped* coarse scan on long-GOP
cheap where §15's arbitrary stride was catastrophic. The proposal's stride is the
**commanded speed**, not an estimate — that is the whole answer to how it avoids the
feedback loop that killed three of §15's four failed gate inferences.

**Corollary for the drag path (owner, 2026-08-10): smooth, responsive scrubbing takes
priority over matching final-frame scaling quality during motion.** Fidelity is owed to the
frame the user stops on, not to the frames flying past on the way there. This resolves a
whole class of trades in advance — preview resolution, preview filtering, sampling stride,
paint pacing — so do not re-open any of them on quality grounds alone. It is what settled
the drag preview staying unfiltered after step 9 sharpened the landing (plan §28.6 item 2),
and it is the principle §15's "sampling may skip frames during an active drag and nowhere
else" was already an instance of. **The owner extended this to the reverse shuttle on
2026-08-10** — "at accelerated reverse speeds, do not require every source frame" — so
accelerated reverse is now the third instance of the same rule, alongside the drag preview
and §15's sampling. Fidelity is still owed to the frame rewind *stops* on.

**THE INTERFACE PASS IS THE OPEN PHASE as of 2026-08-10** — the owner chose it and lifted
the no-interface rule. Spec in `docs/interface-pass-1-spec.md`, assets in
`assets/260807 Trace Media Player Icon/`. **Performance still outranks it**: every phase
runs the playback and scrub regression, and a feature that costs smoothness loses.

**§2 of that spec was RE-DERIVED on 2026-08-10 and is no longer the 2026-08-09 text.** Read
it as it now stands. Four results decide the shape of the work. **Item 2 was stale** —
continuous reverse is built, measured and signed off, so the spec's capability-detect-and-
defer branch is a call site rather than a plan. **Item 1 is materially larger than written,
and the `d3d11` default flip is what enlarged it**: the composited overlay is now the path
that *ships* while `TRACE_RENDERER=cpu` — the documented escape hatch — has no compositor at
all, so a floating transport built only in `OverlayCompositor` would leave the escape hatch
with no transport. It needs a renderer-neutral home. **Item 6 gained a trap from step 9**:
the D3D11 reduction taps come from the reduction ratio, so a 90° rotation must recompute
them from the *post-transform* fit or the box average filters the wrong axis. **Item 8's
premise was half wrong** — video is already zero-based including the right endpoint, but the
image-sequence and still HUD lines print `currentFrame + 1`.

**SPEC PHASE 2 IS DONE (2026-08-10, `58bfca6`); the phase record is
`docs/interface-pass-1-progress.md`.** Fullscreen is a shared `QAction` (F11 listed *first*,
because Qt advertises only the first sequence, with Ctrl+Return and Alt+Enter behind it), the
dev HUD toggle is one too, on **`H`**, and the icon tree is down to the approved `260807`
package. Playback, scrub, `-SnapRelease`, both lifecycle through-drag gestures and all six
shuttle exits are unchanged against a control built from `87a39a6`.

Three things from it worth carrying:

- **`viewState_.showInfo` is DELETED, not wired up.** `Key_I` toggled it and nothing read it,
  so pressing `I` repainted and changed nothing. `showTimecode`/`showSeconds` were dead the
  same way. `Ctrl+I` is the Movie Inspector at phase 12; there is one HUD and it is `showHud`.
- **Hiding the HUD moves `stalls`, and `win WxH` DOES NOT CATCH IT.** The handoff predicted the
  §22.8 effect and named the wrong guard. Measured on one 4K H.264 reversal drag, HUD shown vs
  hidden: `win 1280x843` **both times** — the window does not resize, the *viewer* takes the
  HUD's height. What moves is `display` (**640x360 → 1280x720**), and with it `stalls 70 of
  370 → 127 of 450`. **Quote `display` as well as `win WxH` whenever the HUD was toggled.**
  `hitch` read **1 either way**, which is the fourth time the threshold-independent figure has
  been the one that survived a changed denominator.
- **Artwork follows behaviour, and that is why one asset directory is still here.** The
  approved package has no frame-step icon by design and its `transport_scan_*` pair is the art
  for the *redesigned* Rewind/Fast-forward — which still step one frame until phases 4–5. So
  `assets/Interface/` survives for exactly two glyphs and leaves with them; `transport_scan_*`
  is embedded but unused so those phases are a code change only. The same rule *fixed* the
  overlay, whose side regions drew scan chevrons over stepping behaviour: they carry the
  frame-step glyphs now. Cross-backend agreement is untouched — the cpu-vs-d3d11 diff reads
  **312 px (0.619%), max delta 24 on the control and the same to the pixel after**.
  (**Superseded on the directory point by the asset reorganisation below** — the approved
  package carries a byte-identical copy of the first-pass set as `player-icons/`, so
  `assets/Interface/` never needed to exist. The behaviour rule itself stands unchanged.)

**THE ASSET TREE IS REORGANISED AND EVERY REFERENCE RE-POINTED (2026-08-10, `cbf6d98`).** The
owner moved `assets/` by hand outside a session and nothing that referenced it was updated, so
`rcc` failed on its first entry and the tree did not build. The layout now separates a master
you never edit from working copies named for what they do: `assets/source/original-design-package/`
is the untouched export, `assets/branding/app-icon/` and `assets/interface/{transport,window,common}/`
are working copies, and `assets/README.md` states the rule. `rewind`/`fast-forward` **are**
`transport_scan_reverse`/`transport_scan_forward`, renamed to what they do.

Two things to carry. **The handoff listed 22 dangling references and there were 23** —
`app/trace.rc` pointed at the old `trace.ico` path, so the Windows resource compiler dangled
too and no count of `.qrc` entries would have found it; grep the tree for the old path rather
than trusting an enumeration. And **`interface/` carries the SVG master plus exactly the PNG
renditions the `.qrc` embeds and nothing else**, so the directory listing and the `.qrc` agree
by construction — the absence of that property is what caused this.

**`scripts/verify_trace_assets.py` CHECKS A DELIVERED ICON PACKAGE AGAINST THAT CONTRACT, AND
IT IS A CI STEP AS OF 2026-08-15 (`3e0c936`) BECAUSE ITS SET IS NOW DERIVED.** It asserts every
glyph the `.qrc` embeds is present, every PNG is exactly the pixel size its name claims, the SVG
masters are there, and nothing extra is sitting in the working copies. **`rcc` already fails the
build on a missing entry, so that half is covered; a 25px export named `-24` is not** — it builds
green and renders wrong on one control at runtime, which is precisely the class that is invisible
in a folder listing. It also rejects interaction-state art (`-hover`, `-pressed`), because those
are a brightness multiply applied at draw time and a delivered file would be embedded-and-unused.

**THE EXPECTED SET IS READ FROM `app/resources.qrc` AND `app/trace.rc`, NOT WRITTEN DOWN.** It
used to hard-code the manifest, which made it a duplicate of the contract rather than a reading
of it, so the next `.qrc` edit would have left it stale — and that is the only reason it was kept
out of CI. **`trace.rc` is parsed as well as the `.qrc` because it is a SECOND contract that has
already failed once**: `assets/` was reorganised, the `.qrc` was re-pointed, `trace.rc` was not,
and the resource compiler dangled on a path no count of `.qrc` entries could have found.
Deriving from the file **paths** also deleted the note describing the fullscreen alias remap —
the path is the disk name, so the remap stopped being something the script has to be told.

**Two things it checks that the `.qrc` cannot state, both labelled as such**: the SVG master
beside each embedded PNG (the `.qrc` embeds no SVGs because Trace does not link Qt6::Svg, though
its own comment states the rule), and the **unembedded** app-icon delivery set — macOS
renditions, the extra Windows sizes, the `.icns` — which has no contract file behind it, stays
behind `--app-icon`, and is **deliberately not run in CI**, because putting a hand-written list
there would reintroduce exactly the staleness this change removed.

**CI runs it immediately after checkout, before the ~20 minute build**, as
`--strict --no-pillow`. `--strict` asserts the `.qrc`'s own stated invariant that
`assets/interface/` holds the masters plus exactly what it embeds; it is also the "artwork
follows behaviour" rule, so **a glyph committed before the code that uses it now turns CI red on
purpose**. `--no-pillow` pins the PNG-header reader so the result cannot depend on whether a
runner image ships Pillow — both readers were confirmed to agree on all **176** PNGs in the tree,
and the runner takes the header path (observed: Python 3.12.10, `derived: 25 embedded files`).
The step resolves and prints the interpreter and reports a missing Python **distinctly from an
asset failure**, so it cannot accuse a correct delivery.

**Verified with eleven negative controls plus an untouched copy as the control**, each breaking
the delivery one way: missing PNG · a PNG named `-24` containing 48px · missing SVG master ·
sized state art · missing `trace.ico` · a stray file warning by default and failing under
`--strict` · an unreadable and a malformed `.qrc` both exiting **2** rather than passing. **The
one that matters is a NEW `.qrc` entry being demanded with no edit to the script** — that is the
difference between deriving and duplicating, and it is the only case that would still pass on the
old version. It also closed a recorded soft spot: the state-suffix test strips the size first
now, so `play-hover-24.png` fails where it previously only warned.

**THE 260817 UI v2 ASSET SWAP IS IN (2026-08-17, three commits), AND THE REDESIGN IT PREVIEWS
IS NOT STARTED.** The owner's UI roadmap is annotated at `docs/ui-redesign-roadmap.md` — read
it before any UI work. The delivered package is committed untouched at
`assets/source/260817-trace-ui-v2/` (**renamed before first commit from a folder name
containing a backtick**, PowerShell's escape character — any harness referencing the old path
would have misbehaved). The ten glyphs and the app-icon PNG/SVG sets were swapped in place —
same names, same sizes, **no `.qrc` or code change**, one revertable commit — and
`trace.ico`/`trace.icns` were compiled from the delivered PNG sets since the package declares
them absent: five PNG-compressed ICO entries (16/24/32/48/256) and seven ICNS PNG entries,
**mirroring the existing containers' structure exactly** rather than inventing a layout.
`volume` and `loop` stay in `source/` only — the features do not exist, and `--strict` failing
on them in `assets/interface/` is the "artwork follows behaviour" rule enforcing itself.

Three things to carry.

- **Verification went to the built binary, not the tree**: `--app-icon --strict` exit 0, and
  the five ICO payloads plus the new glyphs confirmed **inside `Trace.exe`** by byte search —
  `rcc` stores PNGs uncompressed, so the embedded bytes are the file bytes and a negative
  search would have meant a stale embed.
- **The cross-backend comparison passed with no pixel-snapping work**: `overlay.ps1` on both
  renderers reads `08-mid-drag` **0 px, max channel delta 1** — the recorded standard — so the
  new art inherits the layout's pixel snap; `banddiff.ps1` in bar mode reads 0.12%/max 29,
  which is the video band's own backend class, not the artwork. Display that session was
  1920x1080 @ 59.999Hz; figures are not comparable to physical-panel records.
- **`powershell -File script.ps1 -Env "A=1","B=2"` FLATTENS THE ARRAY, and the first diff
  compared cpu with cpu and read a perfect 0.** The mangled token gave `TRACE_RENDERER` an
  unknown value (warns, falls back to cpu) and swallowed `TRACE_TRANSPORT_BAR` entirely — both
  "backends" ran `renderer cpu +overlay`, and the check exonerated the build wrongly, the
  harder direction to notice. Caught by reading the HUD's `renderer` field off both captures.
  Invoke `restart.ps1` with `&` from inside PowerShell so the array survives, and **read
  `renderer` off both HUDs before believing any cross-backend diff.**

**ROADMAP STEPS 2 AND 4 ARE DONE AND THE RE-BASELINE IS TAKEN (2026-08-17, `5ff6431` ·
`635656a` · `c63aba4`) — BUT THAT RE-BASELINE IS SUPERSEDED BY STEP 7's, BELOW.** Step 7
(2026-08-18, `10a7fba`) took the menu bar out of the layout too, so `chrome` went `0x21 → 0x0`,
every §4 opening size moved again and the video rect grew; the figures here are a record of the
pre-step-7 chrome, not a comparison baseline. (Original heading continues: display 1920x1080 @ 59.999Hz, so `stalls`' bar is 33.3ms and no figure
is comparable to a physical-panel record.)** The shipping window was menu bar + picture after
this pass and is **picture alone** after step 7: the HUD ships hidden and the status bar is never
constructed in overlay mode. Three commits, separately revertable, non-adjacent edits.

- **Every transient message goes through `MainWindow::showTransientMessage(text, timeoutMs)`
  — the 35 `statusBar()->showMessage` sites were an inventory first, not a deletion.** They
  split into spec-required confirmations (File path copied, the LucidLink five, Copied frame,
  Recent files cleared/removed), validation refusals (Go to Timecode, the scrub-preview copy
  refusal) and ~20 error sites, and none could vanish silently. Routing is on `barIsDocked_`
  — the same single gate that decides which transport is on screen — so bar mode keeps the
  status bar (old chrome, harness comparability) and overlay mode gets a **composited toast**:
  a third image in `OverlayModel` beside the atlas and the rate text (`OverlayHooks::
  messageText`, host-owned string and timer, pill background baked in), **emitted OUTSIDE the
  panel's opacity gate** so a confirmation survives the transport's fade, and synced by
  revision in both drawers like everything else.
- **THE D3D11 QUAD LOOP'S SHAPE WAS LOAD-BEARING AND THE FIRST CUT DREW NOTHING.** Selecting
  the SRV through a pointer-to-`ComPtr` plus a switch made the drawer draw no quads at all —
  the transport panel vanished on the default renderer while the CPU backend was fine — and
  every line of it read as equivalent to the original. Found because the empty-stage capture
  was compared against a control build, then bisected by rebuilding the edit in stages: the
  third `syncTexture` and the guard removal were innocent, the loop shape alone was the
  poison. The shipped loop is the original ternary shape extended by one case. **When a
  mechanical rewrite of working code misbehaves, bisect it in stages against a control before
  reading it harder.**
- **`TRACE_HUD=1` forces the HUD on from launch, and `restart.ps1` passes it BY DEFAULT** —
  every recorded figure is read off the HUD in a pixel capture, so the harness ecosystem
  assumes a visible HUD and that assumption now lives in the common launcher (an explicit
  `TRACE_HUD` entry in `-Env` wins, so `TRACE_HUD=0` measures the shipping look
  deliberately). The six scripts that launch Trace directly and read the HUD (`viewscale`,
  `viewtransform`, `inspector`, `phase14`, `recentfiles`, `sidebyside`) set it themselves;
  `dpimove.ps1`'s launcher forces it so `-HideHud`'s `h` press still means *hide* rather than
  toggling a hidden HUD back on.
- **The §4 chrome term collapsed to the menu bar (`chrome 0x21`), so every opening size moved
  — measured, shipping config**: 16:9 (4K and 1080p) `viewer 1280x720, bound cap` in a
  1296x780 window; 9:16 `460x818, bound minimum`; 1:1 `774x774, bound work`; 4:5 `619x774,
  bound work`. Aspects 1.7778 / 0.5623 / 1.0000 / 0.7997, two-pass convergence visibly doing
  its job on 9:16 and 4:5 (`TRACE_SHAPE_LOG=1`).
- **The re-baseline, all flat against the record's classes** (cadence via `restart.ps1` →
  `TRACE_HUD=1`, `TRACE_NO_AUDIO=1`, scratch INI, ×2 each; 16:9 cadence geometry `win
  728x795` / `display 728x410 filtered x3`): 4K H.264 **100.0/100.0%** (120f, all ~1×,
  `drop 0`, `rephase 0`) · 4444 **99.8/99.8%** (261f) · 1080p **100.0/100.0%** (240f) · 4K
  60fps **100.0/100.0%** (162f at 60.00) · 422 HQ **99.9/99.9%** (168f) · 1×1 and 4×5
  **100.0%** ×2 each. Scrub, bar mode widened to 1280: 4444 `-SnapRelease` `target 261 shown
  261 delta 0` full-res planar, `release 20.8ms`, `hitch 0`, `land 0`, `ui over-16ms 0 of
  898` (`win 1264x843`, `display 640x360 x4`); 4K H.264 reversals `rev-hit 97.9%`, `seeks 3`,
  `hitch 1`, `delta 0`. Lifecycle **38.9% moving / 0% control**. **25 of 25 transitions.**
- **Two harness facts this geometry created.** The §4 default window at this display with the
  HUD shown is **656 logical px wide for 16:9 media, and the docked bar's groove is under
  `scrub.ps1`'s 300px minimum** — so the groove-scanning harnesses now need `widen.ps1`
  (to 1280 here) after `restart.ps1` on *16:9* media too, not just portrait; widening 16:9
  DOES move the video rect, so quote `win`/`display` from the run itself. And
  `transitions.ps1` runs with **`-Env TRACE_TRANSPORT_BAR=1,TRACE_HUD=0`** — it decides by
  picture motion, not HUD text, and with the HUD's chrome gone §4 gives it a window wide
  enough for its control scan.

**ROADMAP STEP 3 IS DONE (2026-08-18, `72aa9ac`): THE POLISHED EMPTY STATE, AND IT REMOVES A
DUPLICATION RATHER THAN ADDING ONE.** The prism mark and its hint line are a **fourth image in
`OverlayModel`**, beside the atlas, the rate text and the toast. The literal
`"Drop media or File > Open"` and the code that drew it existed independently in
`CpuImageRenderer` and `D3D11VideoRenderer`, and **not even as the same mechanism** — a
`QPainter::drawText` on one, a window-sized uploaded texture on the other. `setPlaceholderText`
and `ViewerWidget::setCenterText` left with it; **`setCenterText` had no callers at all**, so
the only thing that ever reached those two literals was their own default.

**IT IS EMITTED OUTSIDE BOTH GATES, AND THAT IS THE DESIGN.** Outside the opacity gate, like
the toast, because it must not fade. Outside **`enabled_`**, because the empty state is what
the window *is* with no media and must survive `TRACE_TRANSPORT_BAR=1` — the documented escape
hatch, which would otherwise be a black window with no hint. Same reasoning as the pan gesture
being ungated in `onMouseDown`: a behaviour that belongs to the window, not to the transport.
Verified in bar mode on both backends — the stage is **641 rows rather than 735** and the mark
is correctly centred in the smaller stage.

**"IS THERE MEDIA" IS ASKED OF THE RENDERER, NOT OF `MainWindow`, AND THAT IS WHAT MAKES THE
NO-FLASH REQUIREMENT STRUCTURAL.** Each backend passes `setMediaPresent()` the same member its
own draw branch reads, so the two cannot disagree. `currentMedia_` would have answered too and
would have been wrong twice: `openPath` releases the outgoing media before the incoming frame
lands, and a **first open that fails** leaves it set with nothing on screen. *There is no
picture* is true at startup, true after Close Media, **false throughout a file change** —
because the viewer holds the outgoing frame until the incoming one lands — and true when an
open failed.

**THE D3D11 VIDEO DRAW IS NOW GATED ON `hasContent_`, WHICH IT WAS NOT BEFORE, AND THE GATE IS
NOT COSMETIC.** `textureSrv_` deliberately outlives `clearFrame()` so the next media does not
pay a texture creation, and until now the placeholder upload overwrote it — so "no frame" and
"the texture holds something drawable" were never both true. With nothing uploaded in its
place, drawing on `textureSrv_` alone would stretch the **outgoing file's last frame** across
the window of a closed one.

**`ViewerWidget`'s overlay fallback is no longer gated on `overlayModel_.enabled()`.** It was,
because the only thing the composited path carried was the floating transport and bar mode had
real widgets doing that job. The empty state is drawn in **both** transport modes now, so a
backend that cannot draw quads can no longer present a window with no media in it whichever
transport is selected. **Reasoned, not measured** — forcing an overlay-pipeline failure needs a
shader that will not compile.

**THE ARTWORK IS A DIFFERENT ASSET FROM THE APP ICON'S PLAY MARK, AND THE NAMES ARE CLOSE
ENOUGH TO REACH FOR THE WRONG ONE.** `assets/interface/branding/empty-mark.svg` is the
empty-state mark from the design package's own `Trace-App-Mockups.html`, section `screen-3`;
`trace-play-mark.svg` is the app icon's and differs in five places — **scale 1.7 not 1.9**, a
five-stop prism edge rather than a two-tone cyan/violet one, edge stroke 11 not 9, glow opacity
0.42 not 0.3, specular 0.6 not 0.7. Rendering the wrong one looks almost right.
`assets/interface/branding/` is a new working-copy folder, so `--strict` and the SVG-master
rule cover the mark the same way they cover a control glyph.

**TWO VALUES IN THE MASTER ARE ANIMATION PHASE RATHER THAN DESIGN.** The package's mockup PNG
is a browser screenshot taken partway through an 18s loop, so **its still and its own t=0 do
not match** — the delivered art at t=0 draws a pink edge where the published still shows cyan.
The animation start is rotated to **150°** and the two animated glow stops carry their own
`values[0]` as their static colour, which makes the still, the animation's t=0 and the shipped
PNG one thing rather than three. Measured against the mockup crop composited over its own
`#050506`: **mean absolute channel delta 0.67, max 39**, against the delivered art's own
**1.82 / 71** at the unrotated phase.

**RENDITIONS ARE 104 AND 208, NOT 24 AND 48**, because the design draws this mark at a fixed
104 logical px — so those are the @1x/@2x pair *at its own drawn size*, exactly the convention
the controls follow at theirs. `paintIcon` takes its candidate sizes as a parameter now, and
every candidate is existence-checked, which subsumes the `-72` special case rather than sitting
beside it. **`verify_trace_assets.py` demanded both new files with no edit to itself** — the
derived set went 25 → 27 — which is the property that whole change was made for.

**THE DESIGN'S SIZES ARE FIXED LOGICAL PIXELS, NOT A FRACTION OF THE WINDOW**: a 104px mark, a
22px gap, a 14px line, `rgba(255,255,255,0.42)`, centred as a column. So the mark stays 104px
in a 460px portrait window and in a fullscreen one. Checked against the delivered mockup rather
than assumed — its client is 832x483 and its mark canvas measures 103, which is 104 at that
render's 0.9917 scale.

**Measured, both backends, 1920x1080 @ 59.999Hz** (so no figure here is comparable to a
physical-panel record): mark ink **59x68** where the mockup reads 59x68 · optical offset
**+10.5px** where the mockup reads +9.5 · hint **157x13** centred **+0.5px** · gap **45px**
where the mockup reads 45 · **cross-backend empty window 0 differing pixels, max channel delta
0** over the whole 1280x735 stage. The mark's own bounding box sits **9.5px right of its canvas
centre by design** — a right-pointing triangle is balanced by eye, not by its box — so what is
centred is the **canvas**, and the offset comes along.

**Regression, against a control built from `893237c` and hash-verified** (`da670347` vs
`020c3757`), same display, same day: 4K H.264 cadence ×3 **99.1/99.1/99.2%** with `drop 0`,
`rephase 0` and buckets `~1x 118 / 1.5-2.5x 1` — **the control reads 99.1% with the same
buckets and the same p50/p95/p99/max**; 4444 `scrub -SnapRelease` `target 261 shown 261 delta
0` full-res planar, `hitch 0`, `land 0`, `release 20.6ms` against the control's 21.4; both
lifecycle legs **40.7% moving / 0% control, identical to the control to the digit**; **25 of 25
transitions**; `overlay.ps1` cross-backend `08-mid-drag` **0 px, max channel delta 0**, with
`renderer d3d11 +overlay` and `renderer cpu +overlay` read off both HUDs first.

**LAST SESSION'S 100.0% CADENCE FIGURE DID NOT REPRODUCE TODAY ON A BINARY BUILT FROM THAT SAME
COMMIT.** The re-baseline recorded 4K H.264 at **100.0%** with `win 728x795` / `display
728x410`; today the same clip through the same harness reads **99.1%** at `win 703x794` on both
the control and the new build, and their captures are **the same size to the pixel**. So the
machine moved, not the code — and the only reason that is known is that the control was built
and run rather than compared against the record. **A recorded figure is a record, not a
baseline, unless a control was taken beside it.** Note the same session's 4444 run reads
`win 728x795`, so the opening size is media-dependent through the HUD's own wrapped height;
quote `win` and `display` from the run itself.

**`scripts/measure/emptystate.ps1` IS NEW, AND NO OTHER HARNESS CAN REACH THIS STATE** —
`restart.ps1` takes a mandatory `-Clip`, so "what the window looks like with no media" had
never been measured here at all. Four modes: `launch` (geometry against the design's own
proportions), `transport` (the panel must still draw over the empty state — the empty-state
half of the D3D11 quad-loop regression check, since step 3 adds a case to that loop; **459x83
on both backends**, the settled panel), `close` (Ctrl+W; **both legs read**, because a check
that only looks at the second one passes on a build that never draws a frame), and `swap` (two
opens; **57 samples across the change, every one showing a picture, the mark never appearing**).

Three things it cost to get right, each a repeat of something this project already knows.
**The mark is found by its colour and the hint by the absence of colour**, never by a layout
constant — the prism is strongly chromatic and the hint is exactly neutral. **The menu bar
defeats both detectors at once**: its labels are subpixel-antialiased, so "File Edit View
Window Help" is a row of strongly *chromatic* pixels, and its background is a lit neutral; the
first run reported a 670x398 "mark" that was the menu bar. And **"the stage is the black part"
is true only of the state the script was first pointed at** — §4 shapes the window to the
media, so with a clip open a frame reaches all four edges, there is no black row anywhere, and
the stage finder returned −1 and indexed off the bitmap. It locates the menu bar's own bottom
edge now, and the hint is scanned in a band **below the mark** rather than across the stage, so
no other neutral-lit thing — a groove, a border, a toast — can join its bounding box.

**The swap detector was proven able to fire before its negative result was believed.** It keys
on a *mark-sized chromatic object* rather than on a coverage band, because a coverage threshold
cannot tell a 59x68 mark from a mostly-dark frame with something colourful in one corner. Run
over a real empty-state capture it reads **bbox 58x68, count 556, fires: True**; run over 57
samples of an actual file change it never fired once.

**THE PRISM ANIMATION IS NOT BUILT, AND THE HANDOFF'S PREMISE FOR IT NEEDS CORRECTING.** The
roadmap called it "a small gradient animator (a `QVariantAnimation` on the stops, or a tiny
shader)". That is true only if the mark is drawn as **QPainter paths and gradients in code** —
which is a *different shipping decision* from the one taken here, not a layer on top of it. The
mark ships as committed PNG renditions embedded through the `.qrc`; re-authoring it as vectors
would leave those files embedded-and-unused, which is "artwork follows behaviour" pointing the
other way, and would give one mark two sources of truth. Animating the PNG instead means either
recolouring it at draw time — which is neither the design's spatial gradient rotation nor its
glow hue cycle — or committing a multi-phase sprite sheet, which at a smooth 18s roll is orders
of magnitude larger than the art it animates. **An owner decision, not a tidy-up.** What still
holds if it is ever built: **one place must decide both "the empty state is showing" and "the
animation is running"**, and that place already exists — `rebuildEmpty`'s `mediaPresent_`
branch, where the image is dropped and its revision bumped.

**ROADMAP STEP 7 IS DONE (2026-08-18, `10a7fba`): THE TRANSIENT TOP CHROME, AND IT FINISHES
STEP 4 — `chrome 0x0`.** The menu bar was the last permanent chrome: with the status bar gone
and the HUD hidden, `windowChromeLogical()` read `0x21` and that 21 was all of it. It now lives
in `src/ui/TopChrome.*`, a strip that **floats over the top of the picture** and is shown and
hidden by the **same reveal state** `OverlayModel` already keeps for the floating transport.
**§4 now reports `chrome 0x0` on every shape** and the picture reaches all four edges of the
client area.

**TOP AND BOTTOM ARE ONE SYSTEM AND THERE IS NO SECOND TIMER.** `OverlayHooks::
setChromeRevealed` is shaped exactly like `setCursorHidden` — the model decides **when**, the
host decides **whether** — and `OverlayModel::syncChrome()` reads one expression,
`targetOpacity_ > 0 || opacity_ > 0`, so the strip arrives at the *start* of the fade-in and
leaves when the fade-out has *finished* rather than blinking out halfway down it. `visible()`
alone could not do it: `opacity_` is still 0 when `reveal()` sets the target, so a strip driven
off that would arrive 16ms late on every reveal. The host's one extra condition is that **with
no media open the strip is held up**, which is what the design package's own empty-state mockup
shows and what stops a fresh launch fading its only menu access away after two seconds.

**THE MENUS STAY REAL `QMenu`s ON A REAL `QMenuBar`, AND THAT IS THE WHOLE ACCESSIBILITY
POSITION.** The composited overlay has no widget tree, which is why phase 14 had to hand-build
a UIA proxy for five transport controls after phase 6. `uiatree.ps1` now finds a **`MenuBar`
with five `MenuItem`s on real rects** beside the five transport proxies — File, Edit, View,
Window, Help — for free, because it is a `QMenuBar`. Gated on **`barIsDocked_`**, the same
single predicate the status bar and the toast already use, so `TRACE_TRANSPORT_BAR=1` keeps
`QMainWindow`'s own menu bar in the layout and the old geometry with it.

Four things it cost, each worth carrying.

- **A NATIVE SIBLING OF THE D3D11 SURFACE WINDOW CORRUPTS THAT SURFACE'S OWN OVERLAY PASS, AND
  THIS IS A THIRD ROW FOR §18.4's TABLE.** As a native child of the **viewer** the strip is
  visible and hit-testable exactly as §18.4 predicts — and the transport panel's **first quad
  draws correctly while every quad after it renders as if its sampled colour were zero**: black
  glyph cells, a black timeline track, on the default renderer only, with
  `TRACE_RENDERER=cpu` perfect. **It is not a data fault and that was established rather than
  assumed**: the atlas texture was read back from the GPU into a STAGING copy and is
  byte-identical to the `QImage` it came from, including an opaque white texel at a pixel that
  drew black, and the uv rects, `alpha` and `brighten` were printed per quad and are all
  correct. **Parented one level up, to the central widget, the fault is gone completely** and
  the strip still covers the top of the picture. So the operative distinction is not "native or
  not" but **where in the HWND tree the native window sits relative to the swapchain's own**.
  Found by bisecting against a control build, not by reading. **The cause is left unattributed
  rather than guessed at.**
- **ALT MUST STILL REACH A MENU BAR THAT IS NOT ON SCREEN, and Qt's own mechanism is what makes
  that a real risk.** A `QMenuBar` gets its `Alt+letter` mnemonics through
  `QWidget::grabShortcut`, and `QShortcutMap` declines a shortcut whose widget is not
  **visible** — so with the strip hidden `Alt+F` would silently do nothing, which is a
  regression for exactly the users phase 14 was built for. An event filter reveals the chrome
  first, and it is on the **window handle** rather than on `MainWindow`: key events reach the
  shortcut map inside `QWidgetWindow::event`, and an object's event filters run before that,
  where `keyPressEvent` runs long after. Nothing is consumed. Measured from a hidden strip with
  `menushot.ps1`: **all five mnemonics reveal the chrome and open their menu**, and a mouse
  click on the strip resolves through `WindowFromPoint` to a Qt window and opens View.
- **THE STRIP IS OPAQUE, AND THAT IS THE DESIGN PACKAGE'S OWN FALLBACK RATHER THAN A
  SHORTFALL.** §18.4 measured that every native-surface variant loses translucency, because
  neither design puts the video pixels anywhere Qt can blend against. The package supplies this
  case's answer — a solid `#14161A` for when Windows transparency effects are off — and the
  backdrop blur it falls back *from* is roadmap step 10, flagged there as the item with real
  presentation risk against a flip-model swapchain. It is native on **both** backends
  deliberately: a translucent strip on `cpu` and an opaque one on the d3d11 default would be a
  real difference inside every cross-backend pixel comparison. Cross-backend, the strip's own
  rows are **identical to the pixel**; the first differing pixel in the whole window is at
  y=70, in the video band.
- **THE EMPTY STATE IS CENTRED IN THE STAGE THE CHROME LEAVES.** The design's own empty-state
  markup puts its stage *below* the 38px strip, and so did the window while the menu bar was in
  the layout; a strip that floats instead would have left the mark half a strip high in the one
  state the strip is permanently shown in. `OverlayModel::setTopInset` is read by the empty
  state and **by nothing else** — the transport is bottom-anchored, and with media open there
  is no empty state — so it can never move a picture. Measured: the mark's optical y offset
  went **−38.0 → −19.0**, against bar mode's own −20.0, identical on both backends.

**FULLSCREEN GETS THE SAME STRIP, AND THAT IS A STATED SIMPLIFICATION RATHER THAN AN
OVERSIGHT.** Verified working there — the strip draws at the top of the screen with the menus
live. The design's screen-2 shows a *different* fullscreen strip: 52px, the filename in bold
beside a dimmer `1920×1080 · 24 fps`, and **no menu bar**. Shipping one strip keeps menus
reachable in fullscreen, which is strictly more functional; the metadata line is new content
and the second layout is new behaviour, so both are left for an owner decision rather than
guessed at.

**The brand mark is `trace-play-mark.svg` from the delivered package, byte for byte, at 15 and
30** — the @1x/@2x pair at its own drawn size, the same convention the controls follow at 24/48
and the empty mark at 104/208. It is a **different asset from `empty-mark.svg` beside it** and
rendering the wrong one looks almost right. `verify_trace_assets.py` demanded both PNGs **with
no edit to itself**; the derived set went **27 → 29**.

**TWO HARNESSES NEEDED CHANGING AND BOTH WERE DEFEATED THE SAME WAY — a detector meeting chrome
it was not written for.** `overlay.ps1` locates the panel by *difference*, and the strip now
reveals with the panel, so the bounding box spanned both and read **1253x675** instead of
459x83; it skips the strip's band now. And `emptystate.ps1`'s mark scan was unbounded at the
bottom on the stated grounds that the chrome down there "is entirely NEUTRAL, so it cannot
enter the chromatic mark scan" — **false in bar mode, and measurably so**: the status bar's
`Ready` is subpixel-antialiased at a **median hi−lo of 115 against the prism mark's own 58**,
so it is *more* chromatic than the thing being looked for and no threshold can separate them.
Unbounded it reported a **1154x470** mark against the design's 59x68 on a build whose empty
state is visibly correct — the same trap that script already records for the menu bar at the
top, arriving from the other end. It has a measured bottom bound now and bar mode reads the
recorded **641-row** stage again.

**THE RE-BASELINE, AND IT IS THE STANDING REFERENCE** (display **1920x1080 @ 59.999Hz**, so no
figure here is comparable to a physical-panel record; a control was built from `d9d4d98` and
hash-verified, `1DCAFEB7` against `1F4F39E9`, and run beside every leg below).

- **§4 opening geometry, shipping configuration (HUD hidden), and every shape moved because
  `chrome` went `0x21 → 0x0`:** 16:9 4K and 1080p **viewer 1280x720, client 1280x720, `bound
  cap`, one pass** · 9:16 **609x1083, `bound work`** (0.5623, two passes) · 1:1 **960x960,
  `bound cap`** · 4:5 **859x1074, `bound cap`** (0.7998). **Three of the four shapes now reach
  the design's area cap instead of being cut off by the work-area bound** — 1:1 goes 774 → 960
  and 4:5 619x774 → 859x1074 — which is what edge-to-edge media buys.
- **Cadence** (`restart.ps1` → `TRACE_HUD=1`, `TRACE_NO_AUDIO=1`, scratch INI, ×2 each; 16:9
  geometry `win 1278x1083` / `display 1278x719 filtered x2`): 4K H.264 **100.0/100.0%** (120f,
  `drop 0`, `rephase 0`, `handler>budget 0 of 119`, all 119 gaps ~1×) · 4444 **99.8%** (261f) ·
  1080p **100.0%** (240f) · 4K 60fps **100.0%** (162f at 60.00) · 422 HQ **99.9%** (168f) · 1×1
  **100.0%** (271f, `display 719x719`) · 4×5 **100.0%** (271f, `display 575x719`). **The control
  reads 100.0% with the same buckets** at `display 1240x698` / `win 1240x1083` — the 38px the
  menu bar used to take, now in the picture.
- **Scrub**, bar mode widened to 1280, `win 1264x1083` / `display 1066x600 filtered x2` on both
  binaries: 4444 `-SnapRelease` **`target 261 shown 261 delta 0`** full-res planar,
  `release 21.2ms` against the control's **21.4**, `hitch 0`, `land 0`, `ui over-16ms 0 of 687`.
  4K H.264 reversals `rev-hit 97.3%`, `seeks 6`, **`hitch 1`**, `delta 0`, `cache 152/152`.
- **Lifecycle 87.2% moving / 0% control**, against the control's **88.0% / 0%**. **25 of 25
  transitions.** `emptystate.ps1` `launch` / `transport` / `close` / `swap` all PASS, plus the
  `-Bar` control.

**TWO HARNESS FACTS THIS GEOMETRY CREATES.** The §4 default window at this display **with the
HUD forced on is bistable**, because the HUD's height is a function of the window's *width* and
the two-pass convergence can settle either way: last session's cadence ran at `win 728x795` and
today's at `win 1278x1083` on both binaries. **The shipping (HUD-hidden) size is not affected**
— it converges in one pass on three of four shapes — so quote `win` and `display` from the run
itself and never compare a HUD-on geometry across sessions. And `overlay.ps1`'s panel location
now skips the top 90 rows; a run that reports a region much larger than 459x83 is reading the
strip and the panel together. **(That 459x83 assertion is superseded at step 5 below — the
transport is edge to edge, so the script asserts the design's 56px height and checks the width
against the captured window instead.)**

**ROADMAP STEP 5 IS DONE (2026-08-18, `15473d0` · `02b07e5`): THE EDGE-TO-EDGE TRANSPORT
STRIP.** The floating 460×84 panel is replaced by the design package's **56px strip across the
full width of the window** — go to start, rewind, play/pause, fast-forward, go to end, mute,
then the timeline between its two readouts, then fullscreen, a separator and share. Nine
controls where there were four.

**THE TIMELINE IS DRAWN, NOT REWRITTEN, AND THAT WAS THE ONE THING THIS STEP COULD GET BADLY
WRONG.** `timelineSlider_` is still the entire scrub state machine; the strip's track is a
*picture* of it, and a press still runs `setSliderDown(true)` / `setValue()` /
`setSliderDown(false)` through `OverlayHooks` exactly as the panel has since phase 6. Nothing
in the strip computes a target, so exact release, latest-target-wins, the drag shuttle and the
press-lands-exactly result are **inherited rather than reimplemented** — confirmed in the code
before the control was designed, and again by measurement after: `-SnapRelease` on 4444 reads
`target 261 shown 261 delta 0` full-res planar, identical to the control.

**THE PHASE 6 PANEL GEOMETRY IS SUPERSEDED, KNOWINGLY, AND IT IS RECORDED AS SUPERSEDING
RATHER THAN DRIFTING.** 460×84 with 44×44 play and 34×34 utility controls was signed off with
*"no tuning is wanted"*, so those were settled numbers. They are now the handoff's **56 / 40 /
36, radius 6, track 4px (6 on hover), thumb 13px (16 while scrubbing), accent `#5AC8E8` on the
played track and the thumb ring and nothing else**. **`kFadeMs` and `kAutoHideMs` are NOT
superseded** and are untouched. Note the package disagrees with itself and the handoff wins:
`HANDOFF.md`'s geometry line says 56/40/36, its mockup *markup* renders 52/38/34. The
arrangement is the markup's exactly.

Six things to carry.

- **FOUR GLYPHS HAD TO BE AUTHORED, AND THE ROADMAP'S PREMISE FOR ONE OF THEM WAS WRONG.** Its
  decision 2 says the package "already carries `volume` / `volume-low` / `volume-muted` art".
  It carries **one** `volume`, in `source/` only, no muted variant, and **no start/end art at
  all**. The retired 260807 set is no help either — **its `prev-clip` is a DOUBLE TRIANGLE**,
  the same shape as `rewind`, so using it would have put one glyph on two controls. The three
  new masters are derived from the delivered ones rather than invented beside them (same
  viewBox, same `#FFFFFF`, same 1.6 round stroke; the pair shares `rewind`'s 6.8..17.2 span and
  go-to-end is an exact mirror about x=12). **There is deliberately no `volume-low`**: with no
  slider it would be art with no behaviour. `verify_trace_assets.py` demanded all eight PNGs
  **with no edit to itself**, derived set **29 → 37**.
- **EVERY CELL IS RASTERISED AT THE SIZE IT IS DRAWN, AND TWO DEPARTURES FROM THAT RULE WERE
  MEASURED BEFORE THEY WERE FIXED.** Cross-backend, over the empty state's **black stage** so
  no video could contaminate it: a narrow gradient column stretched across the window read
  **12,511 differing px at max channel delta 3**; a 1:1 strip cell with the track and separator
  alphas still applied at draw time read **3,594 at delta 2**; with the design's alphas **baked
  into their own cells** — so the only draw-time multiplier left is a fade that is exactly 1.0
  when revealed — it reads **0 px, max channel delta 0**. The reasoning for stretching (the
  gradient varies only vertically, so a horizontal stretch preserves information) is true of the
  *source* and says nothing about the two *samplers*, which is the same mistake the play glyph's
  8.1%/delta-29 offset already cost this project once.
- **A CROSS-BACKEND DIFF TAKEN OVER VIDEO CANNOT SEE THE STRIP AT ALL.** Over a paused frame the
  strip band reads 44.9% differing at max delta 3 — and the *same band with no strip drawn*
  reads 43.9% at delta 4, and the bare video band 65.3% at delta 20. The translucent strip
  *attenuates* the video's own backend difference. **Take strip comparisons over the empty
  state**, which is what `emptystate.ps1 -Mode transport` already sets up.
- **`QAction::toggle()` EMITS `toggled()` AND NOT `triggered()`, AND THAT SHIPPED FOR ONE BUILD.**
  The strip's Mute button calls `toggle()`, because a checkable action must flip its own tick;
  the handler was on `triggered`, so `M` worked and the button moved the tick, changed no glyph
  and never reached the audio. **It was found by a harness assertion, and only after that
  assertion was corrected** — its first version compared the control against a capture taken
  before the pointer arrived, so the hover plate changed all 625 pixels in the cell whether or
  not the click did anything, and it reported PASS on the broken build. Hovered-vs-hovered reads
  43 of 625.
- **A PRESS ON THE STRIP'S OWN BACKGROUND IS CONSUMED NOW, AND THE EDGE-TO-EDGE SHAPE IS WHAT
  MADE THAT NECESSARY.** A press landing on no control starts a **pan** when the picture is
  zoomed (phase 15). That was harmless while the transport was a 460px box; the strip spans the
  window, so the space between the buttons and the readout and the whole right end past Share
  would have dragged the picture out from under the control the user was aiming at.
  Double-click is consumed the same way, or a stray double-click anywhere along the strip
  toggles fullscreen.
- **THE RATE CHIP MOVED, AND THE GEOMETRY FORCED IT.** It sat at the old panel's top-left, in an
  84px panel with an empty corner. A 56px strip has no empty corner — the chip's ~27px would
  land on Go to Start — so "inside the panel" stopped being a position that exists. It is
  **centred above the strip**, which is where the package's §6 puts it and what phase 8 recorded
  as not yet done. Only the position is taken; §6's padding, radius and 900ms/200ms timing
  remain unimplemented.

**Also in the step.** **Home and End are bound**, through the existing `goToFrame()` exact Step
landing Go to Frame already uses — no new decode behaviour. **Mute was promoted from a
`ShortcutTable` key row to a shared checkable `QAction`**, because a button and its key must
trigger one action and the accessibility proxy takes its name from that action. **Nine
accessibility proxies in left-to-right reading order**, the four new ones **interleaved rather
than appended** (appending would have announced Go to Start, the leftmost thing on the strip,
after the timeline); Mute and Fullscreen are `CheckBox`, the honest role for a control that
reports a state. The readouts go through **one shared expression, `readoutTextAt`**, which is
spec phase 7's four modes extracted out of the HUD — so the HUD and the strip cannot print
different values for the same frame, and there is no fifth format.

**THE HOME/END TEXT-FIELD GUARD WAS CHECKED RATHER THAN ASSUMED, WHICH PHASE 7'S RULE REQUIRES
OF ANY NEW SINGLE-KEY SHORTCUT — AND THE MECHANISM THAT COVERS THEM IS NOT THE ONE THAT COVERS
LETTERS.** `QLineEdit`'s `ShortcutOverride` handles *printable* keys; Home and End are not
printable, so what protects them is the modal dialog being a separate window. Measured with its
negative control: **End reaches the end (played track 0.88), Home inside the open Go to Frame
dialog leaves the playhead alone (0.88), Home with no dialog goes to 0.** Note Go to Frame is a
`QInputDialog` **spin box**, whose edit UIA exposes no `ValuePattern` here, so this has to be
tested behaviourally rather than by reading the field.

**THREE OWNER DECISIONS CLOSED THE STEP, ALL 2026-08-18, AND ALL THREE ARE SETTLED RATHER THAN
DEFAULTS.**

- **The three authored glyphs STAY.** `go-to-start`, `go-to-end` and `volume-muted` were
  derived here because the package ships none of them; the owner accepted them "for now",
  which makes them shipped artwork rather than placeholders. **They remain a drop-in swap** —
  same names, same sizes, no code change — if a designer draws replacements.
- **LOOP IS BUILT** (`257a089`), the tenth control, after Mute where the design's own markup
  puts it. **It is the one control whose state is not a second glyph**: the package ships one
  loop glyph where it ships a volume/volume-muted pair, so ON is the accent and OFF is the same
  neutral ink as its neighbours. Its own atlas cell rather than a draw-time tint, because
  `brighten` scales RGB uniformly and cannot turn neutral ink cyan. Its `CheckBox` proxy role
  is therefore doing more work than any other on the strip — colour is the *only* thing
  distinguishing the two states and a screen reader cannot see it.
- **DISABLED CONTROLS STAY VISUALLY UNCHANGED.** With no media, Go to Start and Go to End are
  disabled `QAction`s drawn at full brightness and the click is refused by the action. That was
  already true of every control that predates this step, the package supplies no disabled
  treatment (its own note gives draw-time multipliers for hover and pressed only), and the
  owner confirmed it. **Do not "fix" this** — it is a decision, not an omission.

**THE REGRESSION, AND IT IS FLAT** (display **5120x1440 @ 239.999Hz — the physical panel, NOT
the 1920x1080 the step 7 block was taken on**, so no figure here is comparable to that one; a
control was built from `637c7e5`, hash-verified `578A6C03` against the step-5 `0806244E`, and
run beside every leg).

- **Cadence** 4K H.264 ×2, scratch INI, `TRACE_NO_AUDIO`: **99.1 / 99.1%**, 120 frames,
  `drop 0`, `rephase 0`, `handler>budget 0 of 120`, buckets `~1x 118 / 1.5-2.5x 1`, paints
  208/121. **The control reads 99.1 / 99.1% with the same buckets and the same paints.**
- **Scrub `-SnapRelease`** 4444, bar mode widened to 1280 (`win 1264x1083`, `display 1066x600
  filtered x2` on both): **`target 261 shown 261 delta 0`** full-res planar, `release 21.4ms`
  against the control's **21.1**, `hitch 0`, `land 0`, `ui over-16ms 0 of 706` on both.
- **Reversal drag** 4K H.264: `delta 0`, `seeks 4`, `release ~44-45ms`, and **`hitch 1` on six
  of seven runs against the control's 1 on four of four** — the one run that read 2 is inside
  this gesture's own recorded variance, which is why it was run seven times rather than argued
  about.
- **Lifecycle 83.9% moving / 0% control**, against the control's **82.4% / 0%**.
  **25 of 25 transitions** on both binaries.
- **`emptystate.ps1` all four modes PASS on both backends**, plus the `-Bar` control reading the
  recorded **641-row** stage; `launch` reads mark **59x68**, offset **+10.5**, hint **157x13**,
  gap **45** — the step 3 figures unchanged. `uiatree.ps1` finds **ten** named controls with
  correct roles on the drawn rects (nine before Loop).

**`overlay.ps1` LAUNCHES WITH A SCRATCH `TRACE_SETTINGS_FILE` NOW, BECAUSE IT PRESSES LOOP.**
Loop is persisted, so a run that left it on would poison the next cadence measurement — which
is phase 14's own recorded failure, where a cadence run read a healthy 100% off a single lap.
Its loop leg presses twice and ends in the state it started in, and samples for the **accent**
in the control's cell rather than counting changed pixels, because that is what carries the
state: 0/68/0 accent px on d3d11, 0/54/0 on cpu.

**ONE HARNESS FACT THIS CREATES, AND IT IS A THIRD INSTANCE OF THE SAME TRAP.**
`emptystate.ps1`'s stage bound looked for a row "essentially black" at a mean under 12. The
strip's background is a **translucent dark gradient**, which over the black stage composites to
**6..10** — under that threshold — so the first row tested passed and the bound stopped at the
bottom of the client with the whole strip inside the stage. The mark scan then swept the
strip's **accent**, the first strongly chromatic thing the transport has ever had, into the
mark's bounding box and reported a **433x377** mark against the design's 59x68 on a build whose
empty state is visibly correct. Measured at the strip's top edge the stage reads **exactly
0.00** and the strip **6.00 to 10.77**; the threshold is **3**. `overlay.ps1` was defeated the
other way — it asserted a 460×84 panel and derived control positions as a *fraction of the panel
width*, both of which stop meaning anything on a strip whose width is the window's; it asserts
the design's 56px height, checks the width against the captured window, and derives every
control as a multiple of the measured strip **height**.

**ROADMAP STEPS 6, 8 AND 9 ARE DONE BY AUDIT (2026-08-18), AND THE AUDIT IS THE POINT.** All
three were already satisfied by work predating the roadmap, so they were checked against the
shipped build rather than implemented -- the risk being a later session "implementing" something
that already exists. Step 6: four readout modes reach BOTH strip readouts through one
`readoutTextAt`, measured Frame `1`/`261` - Seconds `0.083`/`10.875` - Elapsed
`00:00:00:03`/`00:00:10:21` - Timecode `00:00:01:16`/`00:00:12:09`, the last being the file's own
`00:00:01:12` start plus the elapsed. Step 8: `kAutoHideMs 2000` / `kFadeMs 165` untouched, and
all three roadmap hold cases plus a fourth are in the timeout handler. Step 9: Escape, geometry
restore, monitor rule and both cursor mechanisms all verified. **One strip windowed and
fullscreen is now an OWNER DECISION (2026-08-18) and the design's screen-2 is deliberately
ignored** -- no 52px variant, no metadata line, menus reachable in fullscreen. Do not re-propose
it.

**THE AUDIT FOUND TWO DEFECTS BESIDE THOSE STEPS AND BOTH ARE FIXED (`b2a901b`).**

- **`QAction::toggle()` DOES NOT CHECK ENABLEMENT AND `QAction::trigger()` DOES.** That is why
  "the click is refused by the action" was true of Go to Start and Go to End with no test in
  their lambdas, and **false for Loop**, the one checkable strip control
  `syncMediaDependentActions` actually gates. Measured before the guard: with no media open,
  clicking Loop on the empty-state strip turned it on -- **0 -> 36 accent px in its cell** -- and
  **persisted `loop=true`** from a disabled action, which also poisons the next cadence run
  (phase 14's own recorded failure). After: **0 -> 0**, nothing written, with the negative
  control confirming Loop still works with media open (**0 -> 46**, persisted). The guard covers
  Mute too, because it belongs to "checkable control on the strip" rather than to whichever one
  is gated today. **This is the second time this exact distinction has cost a build** -- step 5's
  Mute button flipped a tick and never reached the audio.
- **THE READOUT MODE CHANGED THE HUD AND NOT THE STRIP.** `setReadoutMode` rebuilt the HUD, a
  separate widget in the layout; the strip's readouts are quads built inside the viewer's paint.
  **`keyPressEvent`'s `revealOverlay()` is NOT enough** -- `OverlayModel::startAnimation` returns
  early once the fade has settled, so `reveal()` on a strip that is up and idle schedules
  nothing. Measured paused with the strip revealed: pressing `E` **and** choosing the same item
  from the menu both left it reading `0`/`261` at **zero differing pixels** while the HUD read
  `Readout: Elapsed`; four changes in a row never moved it, and an auto-hide plus re-reveal then
  showed the correct values -- **a repaint fault, not a wiring one**. After: **1286 differing px**
  and the strip updates immediately. `update()` rather than `repaint()`, because nothing here
  reports a value measured BY the paint. **Seventh instance of this project's repaint trap and
  the first in the shipping UI rather than the dev HUD.**

**MICA / ACRYLIC CANNOT BLUR THE STRIP, AND THE REASON THAT MATTERS IS NOT THE SWAPCHAIN
(2026-08-18).** Applied to Trace's live main window, `DwmExtendFrameIntoClientArea(-1)`,
`DWMWA_SYSTEMBACKDROP_TYPE` at all four values, the undocumented
`SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)` and legacy
`DwmEnableBlurBehindWindow` **all return S_OK** and change **rows 1..30 only** -- 37,987 differing
px in the title bar and **zero anywhere in the client**, both strip bands included; Mica changes
nothing at all. The HWND tree says why: **`TraceD3D11Surface` is 1280x720 covering 100% of the
client**, and every ex-style is `0` (no `WS_EX_LAYERED`, no `WS_EX_NOREDIRECTIONBITMAP`).

**But the durable half is this: DWM backdrops blur what is behind the WINDOW -- the desktop --
while the design's mockup uses CSS `backdrop-filter: blur()`, which blurs what is behind the
ELEMENT -- the video. Different effects, and no configuration turns one into the other.** That
holds even if the client area were reachable. **Do not re-propose Mica/Acrylic for this.**
Applying it costs nothing measurable (4444 x2: 99.8%, `drop 0`, `handler>budget 0 of 260`,
`hitch 0`, buckets identical) but that is **a null result on a no-op** and is not evidence that a
real transparency route would be free.

**ROUTE 2 IS BUILT AND MEASURED FLAT (`efa3160`), AND IT SHIPS ON AS OF `a4c6bb2` — BUT THE
PAINTED BLUR IS REMOVED ON 2026-08-19 (owner item 8, option B; see that entry below): the strip
rests at layered alpha 215 instead, `StripBackdrop.*` and `TRACE_STRIP_BACKDROP` are gone, and
the alpha sweep measured blur-under-alpha as the worst combination. This block and the three
after it are the record of what was built, not the shipping strip.** The
top strip paints a tiny blurred copy of the video it covers as its background, under the design
package's own `rgba(22,22,24,0.66)` -> `0.04` scrim -- so the design's look is reached **while the
menu bar stays a real `QMenuBar` in a real native window**, which is exactly what route 1
(redrawing the strip as composited quads) would have traded away and what step 7 was shaped to
protect. `src/ui/StripBackdrop.*`.

**THE COST IS SET BY THE OUTPUT AND NOT THE INPUT, AND THAT IS THE WHOLE REASON IT IS
AFFORDABLE.** A 48x6 grid at 4x4 source reads per cell is **4,608 samples a frame at 8K as at
720p** -- the loop is over destination cells. **It must never become a downscale of the band**:
averaging every pixel of a 3840x114 band is ~438k samples at 4K and scales with resolution, which
is the shape of cost this project spends its time removing. Planar YUV at 8/10/12 bits and BGRA
are handled; an unrecognised layout returns null, which means "draw the fallback".

**MEASURED WITH THE CHROME HELD REVEALED FOR THE WHOLE RUN, AND JIGGLED IN THE CONTROL TOO** --
the strip auto-hides after 2s, so a plain 11s run would have measured the effect for two seconds
and the fallback for nine and reported near-zero cost **for the wrong reason**. 4K ProRes 4444 x2
each, 1920x1080 @ 59.999Hz, `renderer d3d11 +overlay` read off both HUDs: **99.8% on all four,
261 frames, `drop 0`, `rephase 0`, `handler>budget 0 of 260`, `hitch 0`, `paints 313/262`
identical**. The captures confirm the strip really was revealed and really was drawing the blur,
so the flat result is not a check that could only report one thing. Both backends draw it;
cross-backend strip band **1847 px of 72576 at max channel delta 7**, under the video band's own
backend class because the scrim attenuates it -- the same attenuation step 5 measured for the
bottom strip.

**THE PROTOTYPE IS GATED ON THE REVEAL STATE (2026-08-18, `2a3c634`), so what would ship is
strictly cheaper than what was measured.** It sampled every presented frame regardless of whether
the strip was visible; it asks `OverlayModel::chromeRevealed()` -- the same state that drives
`OverlayHooks::setChromeRevealed`, so the backdrop cannot disagree with the strip about whether
the strip is up. An ordinary run has the chrome hidden nine seconds in eleven and now samples
nothing for those nine.

**THE GATE'S ONE FAILURE MODE TOOK FOUR ATTEMPTS TO REPRODUCE, AND THREE OF THE ATTEMPTS
CORRECTED SOMETHING.** A gate that stops sampling must publish null, because `TopChrome` keeps
the last image it was given -- but null is only published when the sampler **runs**, and it only
runs when a frame arrives. So a reveal with no frame behind it draws the solid fallback on a
build meant to blur, and `MainWindow::syncTopChrome()` therefore refreshes it on every reveal,
hide and media change. **Stepping frames "while hidden" tested nothing**, because `keyPressEvent`
calls `revealOverlay()` -- the arrow keys changing the frame were themselves holding the chrome
up. **Pausing reveals the chrome AND re-presents a frame.** **Close Media already published
null**, because `closeMedia` calls `setFrame(VideoFrame{})` rather than `clearImage()`, so the
outgoing-file blur that was expected to need fixing **never existed** -- the control binary
corrected that, not reading. What is left is the only reveal that delivers no frame: **playback
running off the end, then a reveal by pointer movement alone** -- control `hsd 0.000` (the
fallback) against fixed `hsd 3.999` (the blur).

**THE COST IS WIDENED TO THE FILES THAT BOUND THE SET AND IS FLAT ON ALL THREE (`586f2b9`)**, on
the physical panel 5120x1440 @ 239.999Hz, so not comparable to the 1920x1080 figures above:
**4K 60fps** (16.67ms budget, the tightest) **100.0/100.0% off against 100.0/100.0% on**; **4K
4444 99.8/99.8% against 99.8/99.8%**; **8K 4444 XQ** (`TRACE_RT_DROP=0`) **50.8/53.5% against
52.2/53.1%**. `drop 0`, `rephase 0`, `handler>budget 0` on both 4K files. **On the 8K the backdrop
rows are nominally HIGHER and the spread WITHIN `bd=0` is 2.7 points against 0.5 between the
configuration means** -- the first run pays a cold decoder -- so that is variance, not a win.
`TRACE_RT_DROP=0` because the drop policy adapts to load and would otherwise turn added cost into
a changed drop count rather than changed throughput.

**Two method facts.** The chrome is held up by **parking the pointer over the picture**, not by
jiggling -- the auto-hide holds while `hover_` is a region, so a stationary pointer inside the
client keeps it up indefinitely (measured still up at 6s at the centre and in the corner, against
~2.6s to hide with the pointer outside), which holds the strip up for the whole run and generates
**no input during it**. And **every run proves its own premise from the same capture the figures
come from**: `strip hsd` is horizontal variation across a strip row, exactly **0** for the
design's purely vertical fallback gradient, reading 0 on all six `bd=0` rows and 3.995 / 2.946 /
17.15 / 16.151 on the `bd=1` rows. **The table is the WORST case deliberately** -- holding the
chrome revealed defeats the gate on purpose. Harnesses: `scripts/measure/stripbackdrop.ps1`,
`scripts/measure/backdropcost.ps1`.

**IT SHIPPED ON (owner, 2026-08-18, `a4c6bb2`) AND WAS REMOVED 2026-08-19 with item 8's resting
translucency** -- the tri-state registry read survives in `TopChrome` gating the resting alpha,
the solid `#14161A` is what the strip paints always (the translucency is the window's), and the
HUD field is `strip` now. **DirectComposition and rebuilding the strip as video quads remain
explicitly NOT to be pursued** (owner, 2026-08-18).

**STEP 10's TYPOGRAPHY HALF IS DONE (2026-08-18, `d91f026`): `src/app/Theme.*`, one home for the
application font, the palette and the popup-menu surface.** `main.cpp` had a hand-rolled grey
palette; `TopChrome` keeps its own screen-1 strip colours because those describe one surface.

- **QT AND GDI DISAGREE ABOUT WHETHER `Segoe UI Variable` EXISTS, AND THE FIRST BUILD SHIPPED THE
  WRONG ANSWER.** GDI lists only the three static optical cuts -- `Display`/`Text`/`Small` -- and
  nothing under the plain name, so the first version mapped the design onto `Segoe UI Variable
  Text`; `QFontDatabase::hasFamily()` declined it and the app silently ran on **Segoe UI**. **Qt 6
  enumerates through DirectWrite and exposes the VARIABLE font under its typographic family
  name** -- exactly one match, `Segoe UI Variable`, the design's own CSS name. Both are in the
  chain, the design's first. **The dev HUD's `font` field is what caught it on its first run**,
  which is why it exists: a fallback to Segoe UI looks very nearly right.
- **The optical cut was settled by the package's own embedded TTF.** The mockup bundles a
  **1.8MB font** -- most of why that one file is larger than all of `src/` -- carrying `wght`
  300..700 and `opsz` 5..36 with a **default optical size of 10.5**, which is squarely `Text`.
- **No point size is set**, applying `TopChrome`'s own reasoning application-wide: pinning 12px
  would look right at 100% and wrong everywhere else, and would override the user's Windows
  text-size setting.
- **The accent goes where Windows puts one and nowhere else.** `#5AC8E8` is `Highlight` and
  `Link` -- text selection -- with `HighlightedText` **dark**, because the accent's luminance
  makes white unreadable on it. It is deliberately **not** the menu highlight: the design uses
  the accent on the played track and thumb ring only, and popups reuse step 7's own
  `rgba(255,255,255,0.10)` so the bar and the popup are one language.
- **Popup menus are the surface step 7 explicitly left to step 10**, and they are **derived** --
  all three mockup screens have no menu open -- from the design's own values: `#1A1B20`, the 1px
  `rgba(255,255,255,0.09)` border, the 8px radius, the menu bar's 3px/8px padding.
  `scripts/measure/themeshot.ps1` puts them on screen for that reason.
- **THE FLUENT ICON DIRECTION NEEDS NO WORK AND MUST NOT BE GIVEN ANY.** The delivered 260817
  glyphs already are it; swapping to `Segoe Fluent Icons` (installed) would put a font glyph
  where a delivered asset exists.

**Regression flat** (physical panel, 4444 control = the same session's `bd=0` rows on the
preceding binary): 4K H.264 cadence x2 **100.0/100.0%** `0 of 119` - 4444 x2 **99.8/99.8%**
`0 of 260`, **identical to the control** - `-SnapRelease` **`target 261 shown 261 delta 0`**
full-res planar, `hitch 0`, `land 0` - **25 of 25 transitions** - `emptystate` all four modes plus
the `-Bar` control, **cpu identical to d3d11 to the pixel** - `uiatree` MenuBar + five MenuItems.
**ONE FIGURE MOVED AND IT IS THE FONT BEING IN FORCE: the empty state's hint reads `169x14`
against the recorded `157x13`, gap 44 against 45.** The hint is *text*; the mark stays `59x68`
with `+10.5` offset because it is a bitmap. **A silent fallback would still read `157x13`.**

**Regression for the two fixes, against a control built from `7c89cee` and hash-verified**
(`959FB800` vs `387E008B`), same display, `renderer d3d11 +overlay` read off both: 4444 cadence
x2 **99.8% on both binaries**, 261 frames, `drop 0`, `rephase 0`, `handler>budget 0 of 260`,
buckets and percentiles identical; `scrub -SnapRelease` **`target 261 shown 261 delta 0`**
full-res planar with **`hitch 0`** on both, release 19.9 vs 21.1ms.

**THE STRIP BACKDROP SHIPS ON BY DEFAULT AND HONOURS WINDOWS' TRANSPARENCY SETTING (owner,
2026-08-18, `a4c6bb2`). SUPERSEDED 2026-08-19 — the blur is removed with item 8's resting
translucency; what survives is the transparency-setting honour, re-purposed to gate the resting
alpha inside `TopChrome`, and the tri-state/HUD reasoning below, which now applies to the
`strip` field.** Roadmap step 10 route 2 was built default-off and left as the last open
owner decision; the decision was to ship it. `TRACE_STRIP_BACKDROP` was **three-valued** --
`0` forces off (the rollback), `1` forces on (the override), **unset asks Windows**.

- **HONOURING THE SETTING IS THE DESIGN PACKAGE'S OWN INSTRUCTION, NOT AN ADDITION TO IT.** The
  package supplies the solid `#14161A` as *"the fallback when transparency effects are disabled
  in Windows Settings"*, so it already expects the setting to be read. Until now that sentence
  was quoted as cover for an opaque strip; it is followed literally. It is also what makes step
  10's Fluent direction Windows-**native** rather than Windows-looking.
- **THE REGISTRY READ IS TRI-STATE (-1/0/1) AND THAT IS THE ONLY AVAILABLE PROOF IT READS THE
  RIGHT KEY.** A path with a typo in it and a machine that has never touched the toggle produce
  the **same boolean**, so a build reading `on` would say nothing about whether it found
  anything. The dev HUD's new **`backdrop`** field prints `on` / `on (unset)` / `off (windows)` /
  `off (env)` / `on (env)` / `n/a`, and on this box it reads **`backdrop on`, not `on (unset)`**
  -- which is what says `HKCU\...\Themes\Personalize\EnableTransparency` actually resolved. Same
  silent-degradation class as `renderer`, `planar` and `font`: the effect is no longer decided by
  the launch, so a run on a machine with transparency off would otherwise measure the fallback
  while its command line said backdrop.
- **`QSettings::NativeFormat` HERE IS REGISTRY *READING*, NOT A SETTINGS HOME.** Spec phase 11
  rules NativeFormat out for Trace's own preferences because it leaves HKCU keys behind after a
  portable install is deleted; that rule is about **writing**. `LucidLinkIntegration.cpp` already
  reads the registry this way for CLSID discovery. Nothing here writes.
- **`refreshBackdrop()` PUBLISHES NULL WHEN DISABLED RATHER THAN RETURNING EARLY**, and that
  became load-bearing the moment the gate stopped being a launch-time constant: `TopChrome` keeps
  the last image it was given, so a gate that merely stopped calling would leave the blur on
  screen after the setting was turned off. One `std::function` call, which is what the reveal
  gate beside it already pays.
- **`WM_SETTINGCHANGE` IS THE ONLY NON-GEOMETRY MESSAGE IN `nativeEvent`, and it does not
  return**, so Qt's own handling of the font and metric changes that message also carries is
  untouched. 30 broadcasts with the `ImmersiveColorSet` lParam: window and client geometry
  identical **to the pixel**, process alive, strip `hsd` unchanged at 2.792.

**Measured, physical panel 5120x1440 @ 239.999Hz, against a control built from `f419957` and
hash-verified (`0B490C0F` against `475F35C7`) and run beside every leg:** default launch with the
variable unset reads HUD `backdrop on` and **strip `hsd` 2.787**; `=0` reads `off (env)` and
**`hsd` 0.00** (the fallback is a purely vertical gradient, so its horizontal variation is exactly
zero); `=1` reads `on (env)` and `hsd` 2.792. Cadence x2 each: **4K 60fps 99.9/100.0%** against
the control's 99.9/100.0% at a **16.67ms** budget - **4444 99.8/99.8%** against 99.8/99.8% -
**4K H.264 99.1/99.2%** against 99.1/99.2%, `drop 0`, `rephase 0`, `handler>budget 0` and
identical buckets on all three. `scrub -SnapRelease` 4444 **`target 261 shown 261 delta 0`**
full-res planar, `hitch 0`, `land 0`, release 21.3 against 21.1ms. `emptystate` all four modes on
both backends plus the `-Bar` control. **25 of 25 transitions.**

**THE TRANSPARENCY GATE IS CLOSED ON HARDWARE (owner flipped the toggle, 2026-08-18), AND THE
WHOLE TRUTH TABLE IS MEASURED. (2026-08-19: the blur this table gated is removed — item 8 —
but the gate itself survives in `TopChrome`, deciding the resting alpha; the registry rows'
meaning carries over as `strip a215` vs `opaque (windows)`, and the env column is now
`TRACE_TOPCHROME_ALPHA`'s pin rather than the deleted `TRACE_STRIP_BACKDROP`.)** With
`EnableTransparency` set to **0** and the variable unset,
the HUD reads **`backdrop off (windows)`** and the strip reads **`hsd 0.00`** at mean RGB
`23.17/24.62/29.17` -- **byte-identical to the `TRACE_STRIP_BACKDROP=0` fallback**, which is what
says the two routes to "off" reach the same paint rather than merely both looking dark. The owner
confirmed the solid strip by eye at the machine.

| registry | env | HUD | strip `hsd` |
|---|---|---|---|
| 1 | unset | `on` | 2.787 |
| 1 | `0` | `off (env)` | 0.00 |
| 1 | `1` | `on (env)` | 2.792 |
| **0** | **unset** | **`off (windows)`** | **0.00** |
| 0 | `0` | `off (env)` | 0.00 |
| 0 | `1` | `on (env)` | **2.792** |

**THE LAST ROW IS THE ONE THAT WAS UNTESTABLE BEFORE and it is the requirement's own words --
the override wins in BOTH directions.** Forcing the effect ON against a Windows "off" reads
`hsd 2.792` at mean RGB `103.45/113.27/123.99`, **identical to the digit** to the same override
taken while the registry said 1. Restoring the setting returns the default row to `hsd 2.787` at
`103.44/113.26/123.98`, this session's own first reading.

**THE LIVE CHANGE WORKS AND WAS CONFIRMED BY THE OWNER RATHER THAN MEASURED HERE.** Trace was
left running with transparency off; flipping it back on **updated the strip with no restart**.
That is `WM_SETTINGCHANGE` reaching `ViewerWidget::onSystemAppearanceChanged()`. The mechanism's
other half is measured: 30 broadcasts carrying the `ImmersiveColorSet` lParam with the value
*unchanged* leave window and client geometry identical **to the pixel**, the process alive and
`hsd` unmoved at 2.792 -- so the handler is live **and** inert on the traffic that message
normally carries. The process the owner tested was closed before it could be sampled from here,
so the live half rests on their observation and on that mechanism, not on a pixel figure.

**`scripts/measure/stripbackdrop.ps1` GAINED `-Mode shipping`** (retired 2026-08-19 with the
script; `topchromefade.ps1 -Mode rest` carries the same principle now), which launched with the
variable **unset** and the HUD on. Setting it to `1` and calling that the shipping configuration
would be testing the override rather than the default, and the two reach the same picture by
different branches. It printed `hsd` beside a capture carrying Trace's own state field, and PASS
required the two to **agree** -- a disagreement is the gate and the drawing having different
opinions.

**ROADMAP STEP 11 IS DONE (2026-08-18): THE BOTH-BACKEND PASS, CONSOLIDATED RATHER THAN REPEATED,
AND IT FOUND ONE DEFECT.** Full record in `docs/ui-redesign-roadmap.md` section 11. Physical
panel, `renderer` read off both HUDs first.

- **`abdiff.ps1` SAMPLES ROWS 6%..46% OF THE CAPTURE -- THE VIDEO BAND -- SO IT CANNOT SEE EITHER
  STRIP, THE EMPTY STATE OR THE TOAST.** Every "cross-backend" figure ever taken with it is a
  statement about the **picture**, not about the chrome. That is why the chrome had to be
  compared separately, and it is the most useful thing the audit established.
- **THE ENTIRE EMPTY-STATE WINDOW WITH BOTH STRIPS REVEALED READS 0 OF 972,800 PIXELS DIFFERING
  AT TOLERANCE 0, MAX CHANNEL DELTA 0** -- top strip, brand mark, wordmark, the real `QMenuBar`,
  the filename, the prism mark and hint, and the whole edge-to-edge transport with all ten
  controls, its timeline and both readouts. One measurement that subsumes the separate step 3, 5,
  7 and 10 chrome claims. Taken **over the empty state**, per step 5's own rule. The popup menu
  body reads **0 of 94,944 px, delta 0** (`themeshot.ps1` gained `-Renderer`; it was hard-coded
  to one backend, which is exactly how `overlay.ps1`'s cpu half went unrun for a whole phase).
- **THE TOP STRIP WITH THE BACKDROP DRAWING, over video -- the one surface that can only be
  compared there -- reads 0 px above tolerance 2 at max channel delta 2**, better than step 10's
  recorded 1847 px / delta 7. **(Superseded 2026-08-19 by item 8: over video the strip band now
  differs 100% across backends BY OWNER DECISION -- d3d11 rests translucent at alpha 215, cpu
  rests opaque -- measured 5434 of 5434 samples at max delta 37. The empty-state figure below is
  the one that still holds, because the empty-state strip deliberately rests opaque.)**
- **SIX PIXELS ARE THE ONLY CHROME DIFFERENCE ANYWHERE AND THEY LOOK LIKE THE FAULT THIS STEP
  EXISTS TO CATCH.** Over *video* the transport band carries six pixels at delta 247, at
  x 116..123 / y 716..727 -- the play glyph's two diagonal edges, white on one backend and
  background on the other. That is superficially step 11's own "8.1% of the play glyph's pixels"
  class. **It is not, and the empty-state result is what says so**: the same glyph is
  byte-identical there. Compositing residue that only shows over a bright background, not glyph
  geometry, and six pixels rather than hundreds.
- **Section 4 opening geometry is identical to the pixel on both backends, all four shapes** --
  16:9 `1280x720`, 9:16 `609x1083`, 1:1 `960x960`, 4:5 `859x1074` -- matching the step 7 record.
  `uiatree.ps1` finds ten named transport controls plus MenuBar and five MenuItems **on identical
  rects to the pixel**. Copy Current Frame returns 4096x2304 on both.
- **The escape hatch is healthy**: cpu 4444 cadence **99.4%** against d3d11's 99.8% and cpu 4K
  H.264 **99.2/99.2%** with identical buckets -- the recorded GATE C class, `sws 16.27` against
  `5.09`. `-SnapRelease` lands exactly on both (`delta 0`, `hitch 0`), release **21.3ms** on
  d3d11 and **91.1ms** on cpu, that path's own recorded cost. Reversal drag `delta 0` and
  `seeks 5` on both.

**THE DEFECT: THE COMPOSITED TOAST WAS DRAWN ENTIRELY BEHIND THE TOP CHROME STRIP (fixed,
`e002085`).** Capturing it on each renderer produced **no toast on either**, while the clipboard
demonstrably held a 4096x2304 frame -- so the command had run and the confirmation had fired.
The message quad is top-left with a 12px margin, chosen at step 2 while the menu bar was still in
the **layout**; step 7 floated the chrome over the picture and did not move it. **It is not a rare
state**: every route to a message is an input and `keyPressEvent` calls `revealOverlay()`, so the
gesture that raises the toast raises the chrome that hides it -- invisible for the first two
seconds of its 2.5s life. Measured both ways on one build: pointer parked **inside** the client
(the auto-hide holds while `hover_` is a region) drew nothing; parked **outside**, the same Ctrl+C
drew `Copied frame 0 (4096 x 2304)` 300ms later.

**`OverlayModel::setTopInset` GAINS ITS SECOND READER, AND THE OFFSET IS UNCONDITIONAL.** It could
have been gated on `chromeRevealed_`, which is right there -- but the 2s auto-hide lands in the
middle of a 2.5s message, so a conditional offset would make the toast **jump 38px partway through
being read**. A constant position sometimes 38px below the design's margin is the better of the
two, and it is the same reasoning that already keeps the message outside the transport's fade.
Both readers are chrome rather than picture, so that setter still cannot move a picture. Bar mode
is untouched -- the message routes to the status bar on the same `barIsDocked_` gate.

**TWO HARNESS FACTS FROM THE PASS.** **`uiatree.ps1` must run with the pointer parked inside the
client**, or the menu-bar half of the walk depends on the auto-hide: a first run read `MenuBar 0`
on cpu against `MenuBar 2` on d3d11 and **looked exactly like a backend difference**; held
revealed, both read the same rects to the pixel. And **any mouse/SendKeys harness is void if
another window takes the foreground mid-run** -- an `overlay.ps1` run had its foreground stolen
and reported `panel-mean 0` with every interaction leg failing at once, which, like
`transitions.ps1`'s 25 identical failures, is a statement about the harness's inputs and not about
the build. Re-run before building a control.

**THE 2026-08-18 OWNER FEEDBACK PASS IS DONE FOR 15 OF 18 ITEMS; THE TOP-BAR GROUP (1's
architecture, 8, 11) IS DELIBERATELY STOPPED PENDING AN OWNER CHOICE.** The triage is
`docs/ui-feedback-260818.md` and the item-by-item record is
`docs/ui-feedback-260818-progress.md` — read both before reopening anything here. **Four items
reverse an earlier decision and each is recorded as the owner deciding again with the thing on
screen, not as drift**: item 4 supersedes §4 item 7's centring (opening media now anchors the
frame's top-left; centring survives only the first shaping of a session); item 5 overrides the
delivered empty-mark art (the INK is centred, measured from the art's own alpha at
rasterisation so replacement art re-centres itself); item 6 reverses phase 14's Loop
persistence (Loop starts off every session, still survives a file change within one; the
`playback/loop` key is never read again — note the poisoned-ini hypothesis was NOT confirmed
on this box, its ini had no loop key at all); item 15 reverses step 5's Go to Start/End
buttons (gone from strip, atlas, `.qrc`, asset tree, hooks, Region enum and the accessibility
proxies — **eight strip controls now** — while Home/End stay bound).

Five things to carry.

- **THE REVEAL CYCLE (items 3+7) WAS THE TOP CHROME'S OWN HIDE, AND THE TRIAGE HYPOTHESIS WAS
  WRONG — fourth session running that a control corrected a plausible reading.** Not the
  cursor-hide feedback loop: the windowed phases cycle with the cursor never hidden. Windows
  posts a synthetic `WM_MOUSEMOVE` at the UNCHANGED coordinate whenever a window's visibility
  changes under the pointer, and the native strip hiding at the end of its own fade is such a
  change — measured with the new permanent `TRACE_REVEAL_LOG=1` (source-tagged reveal/auto-
  hide/chrome/mousemove lines): every `chrome HIDDEN` was followed within 2–5ms by a mousemove
  at the identical pixel, 46 in a run with zero physical motion, a perfect ~2.2s blink.
  **Fix**: a move that does not move is not input (same-coordinate filter in `onMouseMove`,
  reset on leave), plus the pointer resting ON the strip holds the chrome via `holdVisible` —
  the auto-hide's own "never hide under the pointer" rule, which also covers the one case the
  filter cannot (the strip showing over a parked pointer hands the surface a real leave).
- **HIDDEN CHROME NOW MEANS HIDDEN, AND THAT EXPOSED A LATENT SHORTCUT HOLE: `QShortcutMap`
  declines a shortcut whose only widget is not visible, and since step 7 the menu bar lives in
  the auto-hiding strip.** Ctrl+O was the ONE shortcut action not also `addAction`'d onto the
  window and it silently did nothing with the strip hidden — it survived since step 7 only
  because the blink loop kept the strip up ~90% of the time. Hoisted now; every menu action
  with a shortcut must be window-hoisted, and this is the check to run when adding one.
- **Item 18 (console window) was `add_executable(Trace WIN32)`.** The TRACE_*_LOG knobs
  survive via `AttachConsole(ATTACH_PARENT_PROCESS)` at startup, only when the std handles are
  not already valid — so redirection (CI, harnesses) keeps its pipes. **PowerShell does not
  wait for a bare GUI-app invocation and `$LASTEXITCODE` after one is garbage** — ~~CI's
  capture form (`$out = & $exe ... 2>&1`) waits and propagates, verified~~ **CORRECTED
  2026-08-19: that verification was Windows PowerShell 5.1 and does NOT transfer to pwsh 7,
  which CI's selftest steps run in. Three CI runs went red on exactly this — the step threw in
  ~0.5s with `$LASTEXITCODE` EMPTY and zero output lines, starting at this item's commit.
  `Start-Process -Wait -PassThru` with file redirects is the only form that works in both
  editions (fixed in the workflow at `86f1186`, validated in both directions on the shipped
  binary). A harness launching Trace bare and reading the exit code must use `Start-Process
  -Wait`; the capture form is only trustworthy under 5.1.**
- **Item 13**: the D3D11 surface still returns `MA_NOACTIVATE` (child never takes focus) but
  now calls `SetForegroundWindow` on its root first — click-activates, and click-then-Space
  still toggles playback (probed with its negative control).
- **Item 16's thumb pop was item 17's 13→16px cell swap mid-drag** — the scrub-grow variant is
  deleted, one fixed cell, cannot resample. Track ends are rounded (item 2) via half-circle
  atlas cells sliced 1:1 at draw time, baked alphas, both track heights. Readouts carry
  tabular figures (`tnum`) and the position is left-aligned (item 9, superseding the mockup's
  right-align). The open/close toasts are gone, Copy Frame's stays (item 14). Mnemonic
  underlines follow `SPI_GETKEYBOARDCUES` via a `KeyboardCuesStyle` proxy installed in
  `Theme::apply` — the style home moved there from main.cpp (item 10). The strip's mark and
  wordmark are gone (item 1's approved interim; `brand-mark-*` left the `.qrc` and working
  copies, returns at step 12).

**Regression flat, no control binary built — figures compared against their recorded classes**
(physical panel for scrub/lifecycle, 1920x1080-class geometry for cadence; quote from the runs):
4K H.264 cadence ×2 **99.1/99.2%** `0 of 120` identical buckets · 4444 ×2 **99.8/99.8%**
`0 of 260` · 4444 `-SnapRelease` **`target 261 shown 261 delta 0`** full-res planar, `release
20.5ms`, `hitch 0`, `land 0` · **25 of 25 transitions** (one `R -> Space` "no window after
restart" harness flake, re-ran clean — the recorded phase 11 class) · `emptystate` all four
modes both backends plus `-Bar`, mark ink offset **+0.5** (the item 5 target; the harness's
launch assertion now expects ~0, recorded in the script) · `uiatree` eight named controls +
MenuBar/5 items · lifecycle **84% moving / 0% control** · shape selftest 44 rows OK ·
`verify_trace_assets --strict` green through both asset removals with no script edit. **One
expected paints difference**: idle runs with the pointer inside the window no longer blink the
chrome every 2.2s, so paint counts can read lower than older records — that is the bug being
gone.

**ITEM 11 IS CLOSED (2026-08-18, the session after the triage): THE NATIVE STRIP FADES, AND
THE CHEAP EXPERIMENT WON.** Uniform-alpha `WS_EX_LAYERED` + `SetLayeredWindowAttributes
(LWA_ALPHA)` on the strip's own HWND, driven per animation tick from `OverlayModel`'s fade
opacity through the new `OverlayHooks::setChromeOpacity` — same `kFadeMs`, same clock, no
second timer, so top and bottom ramp in lockstep. §18.4's warning applied to *translucency*
(blending a static strip against video pixels Qt cannot reach); a layered fade is composited
by DWM, and **what a mid-fade frame reveals on the d3d11 default is THE VIDEO, not black** —
measured with a pinned alpha (`TRACE_TOPCHROME_ALPHA=128`) over bright footage: the capture
matches `(strip+video)/2` per pixel at **MAE 0.16** against 85.29 for the toward-black
failure mode, and a real hide is a ~165ms ramp with four intermediate frames. Record in
`docs/ui-feedback-260818-progress.md`. Four things to carry:

- **LAYERED CHILD WINDOWS ARE SILENTLY IGNORED WITHOUT A WINDOWS 8+ `supportedOS` MANIFEST,
  AND TRACE NEVER HAD ONE.** The first run measured alpha 128 rendering byte-identical to
  alpha 255 (MAE 0) with the style and attributes correctly applied at the OS level.
  `app/trace.manifest` (merged by the linker beside the default `trustInfo`) is what makes
  the whole mechanism live — it changes the app-wide OS compatibility context, which is why
  the full regression was run against it and read flat.
- **`TRACE_RENDERER=cpu` KEEPS THE POP, GRACEFULLY, AND THE MECHANISM IS KNOWN.** The OS
  state is identical (probed: `LAYERED alpha=128` on both backends) but Qt's native children
  share the top-level backing store, so the rows under the strip hold a baked copy of the
  strip itself — strip blended over strip reads opaque. The strip maps/unmaps at the same
  instants as before, so cpu is visually identical to the pre-fade behaviour; with no media
  the host pins alpha full, so the empty state cannot fade its menu access away.
- **Hit-testing, menus, `Alt` mnemonics and the items-3+7 reveal fix are unaffected** —
  click-on-File and Alt+F-from-hidden both open menus while layered, and a 10s parked-pointer
  `TRACE_REVEAL_LOG` run reads `SHOWN 0 / HIDDEN 1 / synthetic-filtered 1`, identical to the
  fade-off control. **`TRACE_TOPCHROME_FADE=0` is the rollback** (never touches the window
  style); `TRACE_TOPCHROME_ALPHA=N` pins the alpha for capture. Harness:
  `scripts/measure/topchromefade.ps1` (blend / anim / menus / loop).
- **`emptystate.ps1 -Mode transport` HAD A HARNESS DEFECT THIS FOUND**: it revealed with a
  single `SetCursorPos` to a fixed pixel that every run parks the pointer on, so
  back-to-back runs generated no input at all — Windows posts no `WM_MOUSEMOVE` for a cursor
  that did not move (confirmed by reveal log: no line, not even filtered) — and it failed a
  correct build in both fade configurations. It jiggles through two points now. Its `swap`
  leg needs the foreground for dialog typing and is not runnable headless; the control fails
  it identically.

**Item 8 (resting translucency) is NOT closed by this** — at rest the strip is opaque over
the painted blur, which remains the route to the design's look. A uniform resting alpha is
now *possible* on d3d11 but would diverge from cpu; that is an owner option, not taken. The
composited-quads rebuild (option 2) is moot for the fade and no longer justified by it.
**(Taken the next day — item 8 is CLOSED, entry below.)**

**ITEM 8 IS CLOSED (2026-08-19): THE STRIP RESTS TRANSLUCENT AND THE PAINTED BLUR IS
REMOVED — owner decision, option B, accepting that d3d11 and cpu deliberately differ on the
top strip.** The fade's `LWA_ALPHA` ramp tops out at **`TopChrome::kRestingAlpha = 215`**
instead of opaque, so the settled strip shows the real video through itself on the d3d11
default; cpu ignores the layered alpha (the recorded backing-store reason) and rests opaque.
Record in `docs/ui-feedback-260818-progress.md` item 8. Five things to carry.

- **THE ALPHA CAME FROM A LEGIBILITY SWEEP, NOT THE DESIGN'S CSS** (owner instruction — the
  CSS scrim and a uniform window alpha are different mechanisms). 155..255 × blur on/off over
  the two hardest bands in the asset set (the 4K milk splash and the Marinelaverse end tag's
  bright saturated detail), menus open, `TRACE_TOPCHROME_ALPHA` pins on the pre-change binary
  so the sweep ran before any code moved. 230 barely reads translucent, 200 goes marginal
  where a near-white element crosses a label, **215 keeps every label separable on the worst
  frame**.
- **THE SWEEP INVERTED THE BLUR INTUITION, AND THAT MEASUREMENT IS WHY StripBackdrop IS
  DELETED RATHER THAN COMPOSED.** The painted blur is itself a bright copy of the video, so a
  resting alpha under it counts the video twice and washes the labels out by a215 — while the
  solid dark strip content under the same alpha composes into a uniform scrim that is MORE
  legible over bright footage than the shipping blur-at-opaque was. Translucency alone reads
  well; translucency plus blur reads worse than either alone. `StripBackdrop.{h,cpp}`,
  `TopChrome::setBackdrop`, `ViewerWidget::refreshBackdrop`/`setBackdropSink`,
  `TRACE_STRIP_BACKDROP`, `stripbackdrop.ps1` and `backdropcost.ps1` all left. **Do not
  rebuild the blur to compose with the resting alpha — that combination is the measured worst
  case.**
- **THE WINDOWS TRANSPARENCY HONOUR SURVIVES, RE-PURPOSED**: the `EnableTransparency`
  tri-state read lives in `TopChrome` now and gates the resting alpha — setting off → the
  strip rests opaque, exactly the package's own "solid #14161A when transparency effects are
  disabled" case; `WM_SETTINGCHANGE` re-applies it live. The HUD's `backdrop` field is
  **`strip`**: `a215` / `a215 (unset)` / `opaque (windows)` / `aN (env)` /
  `opaque (fade off)` / `n/a` in bar mode. Verified: d3d11 resting band matches
  `strip*(215/255) + video*(40/255)` at **MAE 0.21** against 22.61 for opaque; cpu reads
  opaque at MAE 0.1. `topchromefade.ps1 -Mode rest` is the check, and **its cpu leg's PASS is
  "OPAQUE"** — the accepted divergence is written into the harness as the expectation.
- **THE EMPTY-STATE STRIP DELIBERATELY RESTS OPAQUE** (gated on `mediaTitle_` being empty):
  nothing is behind the empty stage for translucency to show, and it preserves the one
  surface byte-comparable across backends — re-measured after the change at **0 of 972,800 px,
  max channel delta 0**, the step 11 standard intact as a working instrument. **The accepted
  divergence is over VIDEO**: the strip band there reads **5434 of 5434 samples differing at
  max delta 37** across backends while the video band below reads 25 of 61,864 — so a
  cross-backend diff of any revealed state must exclude the strip band (abdiff's 6% band
  start clips the strip's bottom rows and reads ~0.7% from that alone; `overlay.ps1`'s header
  records it). **The strip-band difference is this decision, never a defect to reconcile.**
- **`overlay.ps1` HAD BEEN STALE SINCE ITEM 15 AND ACCUSED A CORRECT BUILD IN PASSING** — it
  still mapped the ten-control strip, so on eight controls every left-cluster aim landed one
  control off: its "go-to-end" tap toggled Loop and its "loop" tap sampled the frame readout,
  two FAILs on a correct build. Re-pointed to the eight-control offsets; its Go to Start/End
  leg drives `Home`/`End` (where item 15 moved the behaviour) with the same played-track
  observable. All legs PASS on both backends, loop accent 0/68/0 — the recorded figure.

**Regression flat** (1920x1200 @ 59.999Hz, the Parsec-class display — no figure comparable to
a physical-panel record; captures read the composited framebuffer so pixel judgements stand):
4K H.264 cadence ×2 **99.1/99.1%** `0 of 120` identical buckets · 4444 ×2 **99.8/99.8%**
`0 of 260` · 4444 `-SnapRelease` **`target 261 shown 261 delta 0`** full-res planar,
`release 21.9ms`, `hitch 0`, `land 0` · reversals `rev-hit 97.3%`, `seeks 6`, `hitch 1`,
`delta 0` · **25 of 25 transitions** (first run on `Splash_1.mp4` failed `F -> ffBtn` at
`moved 0%` — the recorded 121-frame-clip artifact, clean on the header's named clip) ·
lifecycle **83.5% / 0%** · `emptystate` all modes both backends plus `-Bar` ·
`topchromefade` rest/anim/menus/loop, the loop leg identical to the item 11 record.

**STEP 11 IS RE-CONSOLIDATED AT HEAD AND THE BETA RE-CUT IS PREPARED, NOT CUT (2026-08-19,
`86f1186`; physical panel 5120x1440 @ 239.999Hz).** The full re-consolidation record is the
banner at the top of roadmap §11 — read it there. Items 8/11/15 had changed what the step 11
record asserts (eight controls, not ten; the strip-over-video row measures a deleted
mechanism), and the audit found four real gaps at HEAD, all closed: popup **View** menu body
**0 of 99,876 px, delta 0** across backends (re-checked because item 10 moved the style home
into `Theme::apply`); `uiatree` on **both** backends reading eight controls + MenuBar + five
MenuItems + the filename **on identical rects to the pixel**; §4 shipping-config opening
geometry 16:9 `1280x720` / 9:16 `609x1083` **identical on both backends**; escape-hatch health
cpu 4444 cadence **99.4% ×3** (`1 of 260`, the recorded class to the digit — one cold-start rep
read 94.6%/`drop 11` and did not reproduce), cpu 4K H.264 **100.0% ×2** (`0 of 119`), cpu
`-SnapRelease` **`delta 0`**, `hitch 0`, release 45.5ms with the landing async. Copy Current
Frame: clipboard 3840x2160, toast drawn with the chrome hidden.

Four things to carry.

- **CI WAS RED FOR THREE RUNS AND THE CAUSE WAS ITEM 18's OWN VERIFICATION NOT TRANSFERRING
  BETWEEN POWERSHELL EDITIONS.** The selftest steps run in **pwsh 7**, which does not wait for
  a bare GUI-subsystem invocation *even in the capture form* — `$LASTEXITCODE` EMPTY, zero
  output, thrown in ~0.5s, from the commit that linked Trace as WIN32. The capture form was
  verified under 5.1, a different shell than CI uses. Fixed at `86f1186` with `Start-Process
  -Wait -PassThru` + file redirects, validated in both directions locally (exit 0 with the
  selftest line; exit 4 propagated with its FAIL line). **Any step or harness that launches
  Trace bare and reads an exit code must use `Start-Process -Wait`.**
- **CI IS READABLE FROM THIS BOX WITHOUT `gh`.** `git credential fill` yields a token the
  GitHub REST API accepts (`Authorization: token …`) — run status, per-step conclusions and
  the raw step logs are all reachable with `curl`. Every prior handoff said CI could not be
  read from here; that was "no `gh` CLI" hardening into "no access". After the fix, the run at
  `86f1186` is green with **all five verification steps read individually**: dependency gate ·
  FFmpeg + audio detected · `6 required files present, 95.4 MB total` · **`renderer=d3d11
  fellback=0 planar=1`** · `OK - 11 shapes x 4 scale factors`.
- **THE SAME-COORDINATE REVEAL TRAP CAUGHT A THIRD SCRIPT-SHAPED VICTIM — ad-hoc harness code
  this time.** Parking the pointer at the client centre with one `SetCursorPos` revealed
  nothing because the pointer was already there (the items-3+7 filter: a move that does not
  move is not input), and BOTH backends' UIA walks read no MenuBar — which looks exactly like
  the accessibility regression it isn't. Jiggle through two points before any walk or capture
  that needs the chrome up. `emptystate.ps1` already records this; it applies to *any* code
  that reveals by pointer, not just that script.
- **THE RELEASE STATE**: `docs/release-body-draft.md` is re-drafted at `86f1186` and
  supersedes the 2026-08-18 draft, which described the deleted blur and a ten-control strip.
  Version `0.3.0` and all three stage literals verified against the **HEAD binary** (`(beta)`
  UTF-8, `Beta.` capital — a lowercase-only search misses it — and the mail subject; `0.2.0`
  absent). Checklist items 1–3 are done; **the one open item is the owner's look at the
  finished interface at the machine**, and the resting translucency landed last so it has not
  been seen settled. **To cut: copy the draft over `docs/release-body.md`, commit, tag
  `v0.3.0-beta.1`** — the workflow publishes `docs/release-body.md` as the release body.
  Step 12 (frameless window) remains the owner's to schedule.

**SPEC PHASE 3 IS DONE (2026-08-10, `4de678e`).** `keyPressEvent`'s flat switch is a
**`ShortcutTable`** (`src/app/ShortcutTable.*`) and `keyPressEvent` is two lines, because
phase 13 has to render a Keyboard Shortcuts window and a switch cannot be enumerated. **The
table is complete and the dispatcher is not, and that separation is the design**: rows carrying
a `QAction` are documentation only — Qt dispatched them before `keyPressEvent` was reached —
and they point *at* the action rather than copying its keys, so a changed binding cannot leave
the table stale. The dispatcher **matches on the key and ignores modifiers**, exactly as the
switch did; every modifier'd shortcut in Trace is already on an action, and that is the rule.

**`startShuttle()` is the five-step sequence J and L each wrote out**, extracted *before*
phases 4–5 add the buttons as a third caller. **One predicate decides three things**:
`ordinaryForwardPlay` (forward at exactly 1×) is the case that keeps the play intent, the case
that gets sound, and the case that does *not* become a shuttle run — visible from outside, a
default `L` reads `shuttle idle` while the same press under the 2× convention reads
`shuttle RUN FWD stride 2`. `PlaybackController` gains **`ShuttleEntry::AtOneX`/`AtTwoX`**,
applied at the first rung only, so the buttons' 2× entry is an argument rather than a call site
writing `speed`. `TRACE_SHUTTLE_ENTRY=2x` drove it through J/L, which was the only way to
execute it before those buttons existed; **it left with spec phase 5**, which gave both buttons
to `AtTwoX` as an argument and made J and L name `AtOneX` literally.

**SPEC PHASE 4 IS DONE (2026-08-10).** The visible forward control is **Fast-forward**, entering
the ladder at **2×** through `ShuttleEntry::AtTwoX`, with `transport_scan_forward` artwork on
both the transport bar and the composited overlay's right region. `nextFrameAction_` survives
untouched with the Right arrow as its only surface — the spec removes the *button*, not the
command — and it survived without care being taken because phase 3 had already collapsed the
two step paths into one action. `next-frame` left the asset tree in the same commit;
`prev-frame` stayed until phase 5, so for one commit `OverlayHooks` read `stepBack` beside
`fastForward` and **that asymmetry was the rule working**, not an oversight. Measured on the
button: **+2× → +5× → +10× → +30×**, six rapid presses ending on `stride 30`. The spec's
temporary rate indicator is a fixed-width label driven from `startShuttle`, gated on the same
`ordinaryForwardPlay` predicate that decides whether there is a run.

**SPEC PHASE 5 IS DONE (2026-08-10) and the transport redesign is complete.** The visible
backward control is **Rewind**, entering at **−2×** through `ShuttleEntry::AtTwoX`, with
`transport_scan_reverse` on the transport bar and the composited overlay's left region;
`OverlayHooks::stepBack` is `rewind`, `prev-frame` left the tree, and **`TRACE_SHUTTLE_ENTRY`
left with it** — both buttons pass `AtTwoX` as an argument now and J/L name `AtOneX` literally.
`prevFrameAction_` survives with the Left arrow as its only surface, so "frame stepping becomes
keyboard-only" is literally true rather than half true. Both ladders confirmed from the button:
**+2/+5/+10/+30 and −2/−5/−10/−30, six rapid presses capping at ±30×.**

Three things to carry. **The transition axis was re-derived a third time and the negative
control is the point**: `R -> prevBtn`/`F -> prevBtn` became `R -> rewBtn`/`F -> rewBtn` with
their expectation flipped from `still` to `moving` (left as they were they would have asserted
that pressing Rewind stops playback); `R -> Left`/`F -> Left` are where the old coverage went,
not new cases; and `-Delayed` was re-pointed at the arrow key rather than deleted, because the
button was never the point — a step leaves run state that only the *next* run-ending command
exposes. **25 of 25 PASS on phase 5, and exactly the four `rewBtn` cases FAIL on a control
built from `e559d07`** with all 21 others identical. **The ladder cap leg could not pass on any
build**: `Click` spends ~210ms of dwell per press, six presses spanned ~1.6s, and at 30× a
412-frame 24fps clip is traversed in **0.57s** — so it captured an ended run and read
`speed 2.00x` at `frame 406`, which looks exactly like a ladder that wrapped. `FastClick` plus
no settle fixed it. And **the overlay's re-pointed left hook was executed, not just wired**:
state 07 reads `speed -2.00x | Reverse Play` on **both** backends, with `08-mid-drag` still
**0 px, max delta 1** across them.

**That session ran on a 1920x1080 @ 59.999Hz display, not the panel**, so its figures are not
comparable to the phase 2–4 tables; the control was rebuilt and measured on the same display.
Regression flat: cadence 100.0% both with identical buckets, 4444 99.8% both, `-SnapRelease`
`delta 0` and `hitch 1` both, reverse 1× 100.0% on all six runs, forward 2× identical,
lifecycle both legs passing. `land` reads **0 through every press**.

**`landPreviousExactly` IS SETTLED AND GONE: no shuttle press lands the previous run.** K,
Space and running off the end still land, because fidelity is owed to the frame you *stop* on.
Both halves of the old justification failed. **"L must pass true or the lease and queue would
strand" was never about this flag** — `endShuttleRun()` reclaims the lease and clears the queue
*above* its `landExactly` branch, and `startShuttle` calls it unconditionally. And **"J passes
false because a forward run supersedes the picture immediately" described a mechanism
`dd21fe9` removed**: forward is a queued, strided run now, the same shape as reverse. What was
left was *anchoring*, and it was measured (`scripts/measure/shuttleland.ps1`): **4K H.264
−1×→+2× `land 0.8ms`, 1080p −10×→+2× `0.3ms`, ProRes 4444 −1×→+2× `25.2ms`** — and in every
case the forward run that follows is identical (48 vs 48 frames, `starve 0` both, 100.2 vs
100.1%; 4444 46 vs 45 frames, starve 4 vs 5). **The landing is a reverse-cache hit by
construction** — the reverse run decoded that frame moments earlier — and **a cache hit sets
`currentFrame_` but never `lastDecodedFrame`, so it does not move the decoder at all.** There
is no anchor to buy. Proof: at −1×→+1×, ordinary playback's first UI-thread tick pays the same
~105ms walk with the landing as without. The HUD's **`land N (Xms max Yms)` is retained and
reads 0 through any press**, so a regression back to press-landing is visible.

**`revtransitions.ps1` IS REPLACED BY `transitions.ps1` (phase 4), and the axis is
re-derived rather than extended.** Six ways *out of a reverse run* stopped being the right
question when the forward button became a shuttle **entry** — a press that starts a run ends
the previous one in the same call. The axis is now a **run boundary** from each state a run
can be in: **21 cases, all PASS**, including the whole forward row (there was no forward run to
leave before), `R → J` and `F → J` (a same-direction rung change is a full boundary), and
`F → prevBtn → K`, the untested mirror of the gesture that found the phase 3 bug. **Two harness
faults produced passes that meant nothing**: a 9:16 clip pillarboxes four fifths of the picture
signature onto black (13–15% "moving" against 48–49% on a 16:9 clip, and one step reading 0.0%,
both runs PASSING), and a 121-frame clip lets a +2× run reach the tail inside the observation
window and report `moved 0%`. **The clip is part of the measurement.** Button positions are
found by scanning for icon pixels and asserting exactly three clusters — arithmetic off the
groove was wrong by ten pixels, because QSlider insets its groove by the handle radius, and a
formula cannot notice it has drifted.

**Phase 5 re-derived it a THIRD time, and this is now the standing pattern rather than an
incident.** The backward button stopped stepping, so `R → prevBtn`/`F → prevBtn` **kept their
names and changed their meaning** — they are `R → rewBtn`/`F → rewBtn` now and expect `moving`,
where before they expected `still`; left alone they would have asserted that pressing Rewind
stops playback. `R → Left`/`F → Left` are **where the old coverage went**, not added cases, and
`-Delayed` was **re-pointed at the arrow key rather than deleted** because the button was never
the point: a step leaves run state that only the next run-ending command exposes. 25 cases, all
PASS — and **exactly the four `rewBtn` cases FAIL on the phase 4 control**, with the other 21
identical, which is the check that the matrix tests the change at all.

**A harness can also be unable to PASS, and that is harder to see than one that cannot fail.**
The ladder cap leg presses six times and captures once, to show the sixth press reads 30× and
not the first rung. `Click` costs ~210ms of dwell per press, so six span ~1.6s — while at 30× a
412-frame 24fps clip is traversed in **0.57s**. It was capturing an ended run and reporting
`speed 2.00x` at `frame 406`, which is exactly what a wrapped ladder would look like. `FastClick`
(45ms) with no settle before the capture brought it inside budget; both legs then read ±30×.

**`overlay.ps1` WAS AIMING 16px LOW AND HAD BEEN FOR A PHASE** (found at phase 4). It predicted
the panel from `0.485 × window height` — the bottom of the video surface, which moves whenever
the HUD gains or loses a line. Every click landed **1.2px below the icon rect**: the captures
looked right, the panel-mean printed, and **not one interaction leg registered**. The panel is
located by *difference* now (what changes between the hidden and revealed captures) and its
size asserted. **This re-reads the phase 2 overlay number**: with nothing registering, all
twelve captures were the same paused frame, so the recorded `312 px (0.619%)` is the **video
band's own backend difference**, not overlay agreement. With the legs live, `08-mid-drag` —
panel and dragged handle on screen — reads **0 px, max delta 1**, and states 05–07 differ by
18–49% purely because each backend is on a different *frame*. `overlay.ps1` takes `-Renderer`
now; hard-coding `d3d11` meant the cpu half needed a script edit, which is how a check stops
being run.

**The frame-step BUTTON never ended a shuttle run, and that was a real bug.**
`revtransitions.ps1` enumerated six ways out of a reverse run and every one was a key or the
slider; the buttons were a **seventh** and nothing exercised them. Clicking Prev Frame during a
reverse run left `shuttleRunActive_` true with `shuttleLastPresented_` holding the *shuttle's*
frame, so the next K took `endShuttleRun`'s landing branch and **discarded the frame the user
stepped to** — measured against a control from `cbf6d98`, the picture moves **17.6% on that K
press before and 0% after**. **The obvious gesture does not find it**: reverse → click →
arrow-key passes identically on both builds, because it neither hangs nor freezes. Both step
paths are one command now (`stepOneFrame`). Regression flat: cadence 99.9 → 100.0% with
identical buckets, `-SnapRelease` `delta 0` and `hitch 0` both, reverse 1× and forward 2×
identical to the digit, all six transitions and both lifecycle legs passing.

**SPEC PHASE 6 IS DONE (2026-08-11) and the floating transport is now THE transport.**
`transportBar_` is out of the `QVBoxLayout`; `OverlayModel::enabledByEnvironment()` decides
for the whole application, so `MainWindow` (dock the bar?) and `ViewerWidget` (draw the
overlay?) cannot disagree and **no combination of knobs leaves the window with no transport**.
`TRACE_TRANSPORT_BAR=1` restores the docked bar — the escape hatch, the negative control, and
what the eight groove-scanning harness scripts need to keep running. The bar OBJECT stays
alive either way, because `timelineSlider_` is its child and is the entire scrub state
machine; the overlay drives the real slider and the slider is simply not on screen.
Also shipped: the approved package's **44×44 play / 34×34 utility** geometry in a 460×84
panel, the auto-hide reveal and hold rules, cursor hiding in fullscreen, and fullscreen
consolidation (Escape, geometry restore, maximize kept distinct, double-click).
**`TRACE_RENDERER=cpu` keeps its transport — verified on both backends, not assumed.**

Five things to carry.

- **A rapid second press on an overlay control was being DROPPED, and the docked bar could
  never have shown it.** Windows sends down, up, DBLCLK, up, so the second press of any pair
  inside the double-click interval arrives as `WM_LBUTTONDBLCLK`, not `WM_LBUTTONDOWN`. The
  first cut consumed it over a control. `QWidget::mouseDoubleClickEvent` forwards to
  `mousePressEvent`, which is why Qt's buttons were always fine — and why this reads as an
  overlay-only ladder bug. Six rapid presses measured **±10× before the fix and ±30× after**,
  against `scripts/measure/overlay_ladder.ps1` with the fix reverted as the negative control.
  10× is three rungs of six presses: exactly one lost per pair.
- **THE VIDEO RECT DID NOT MOVE, and the handoff predicted it would.** At the default startup
  size the **window** shrinks instead — it is sized from the layout's own hint and the viewer
  keeps its 640×360 minimum. 4K H.264 `win 1280x843 → 1280x767` with `display 640x360 →
  640x367`; 4444 `win 1280x843 → 1280x760` with `display 652x367` **unchanged**. That is why
  no stall or cache figure moved, and it is an explanation rather than an observation. At a
  **held** window size the rect would grow, so a maximized window is where to look if a scrub
  number is ever questioned. Quote `display` either way; the HUD now names the transport too
  (`+overlay` / `+bar`).
- **Plan §31.5 item 2 is CLOSED: the overlay's timeline press lands exactly.** Measured with
  the playhead deliberately far from the press point, which the item required —
  `overlay_press.ps1`, from frame 0, one click at 0.85: overlay `target 101 shown 101
  delta 0`, groove control `target 102 shown 102 delta 0`, both full-resolution planar, both
  one seek plus a GOP walk. The one-frame difference is a 404px track against an 827px groove.
- **`GetCursorInfo` says the CPU backend does not hide the cursor and it does.**
  `Qt::BlankCursor` is a real cursor with an empty bitmap so `CURSOR_SHOWING` stays set; the
  D3D11 surface answers `WM_SETCURSOR` with `SetCursor(nullptr)` and reads `flags=0`. The
  **handle** separates them (`0x10003 → 0x6470DA7 → 0x10003` on cpu). Two mechanisms, one
  behaviour, and the obvious instrument sees only one.
- **Escape is a second SURFACE onto `fullscreenAction_`, not a second definition**, and it is
  a separate `QAction` rather than a fourth shortcut because **a disabled QAction does not
  consume its shortcut**. "Escape means this only while fullscreen" is therefore enablement
  rather than a branch inside a handler that has already swallowed the key — and it could not
  live in `ShortcutTable`'s plain-key half, whose dispatcher consumes unconditionally.
  Verified against the window manager: F11 → fullscreen, Escape → the pre-fullscreen
  rectangle exactly, a second Escape → no change; maximized survives the round trip as
  MAXIMIZED. Double-click needed `CS_DBLCLKS` on the surface window class or
  `WM_LBUTTONDBLCLK` is never sent.

Regression (control built from `fec93f0`, hash-verified on every swap, **1920x1080 @
59.999Hz display, not the panel**): bar mode is flat against the control on every run — 4K
H.264 cadence ×3 99.1→99.2% with identical buckets, 4444 99.8%, reverse 1× 100.0% ×3 at 114
frames / 4.75s, `-SnapRelease` `delta 0` and `hitch 0`, both lifecycle legs, **25 of 25
transitions case for case**. Overlay mode ships at the same numbers; its only measurable cost
is **paints** — 152/121 against 120/121 on playback and 559/469 against 440/441 on a drag, at
0.02–0.05ms each against a 41.67ms budget — and **4444, the file with the least headroom,
absorbed them at `handler>budget 0 of 260`**. Cross-backend `08-mid-drag` still **0 px, max
delta 1**. The `ui gap max` asymmetry reproduced (9.6/7.3 vs 76.0/72.9ms) and is **still
unattributed — not an overlay win**.

**PHASE 6 IS SIGNED OFF (owner, 2026-08-11) and nothing about the floating transport's feel is
open.** The panel clearly reads as the transport, the 2s inactivity delay feels right, the
165ms fade feels natural, and **no tuning is wanted** — so `kFadeMs`, `kAutoHideMs` and the
460×84 panel with its 44×34 controls are **settled numbers rather than defaults**, and
changing one reopens an owner decision. **THE PANEL GEOMETRY WAS REOPENED AND REPLACED AT UI
REDESIGN ROADMAP STEP 5 (2026-08-18)** — an owner decision, recorded there as superseding this
sign-off rather than drifting from it: the transport is an edge-to-edge 56px strip with 40px
play and 36px other controls now. **`kFadeMs` and `kAutoHideMs` are NOT superseded** and this
sign-off still governs them and the auto-hide's feel. Read it at its stated width: what was accepted is the
**auto-hide's feel and the panel's identity as a transport**, not the Time Display readouts
(phase 7 rewrites them), not the menus (phase 13), and **not the overlay as finished** — plan
§31.5 item 4 stands, and it is not final until a screen reader has driven one.

**SPEC PHASE 7 IS DONE (2026-08-11): the time readout is honest and Trace has its first text
field.** `Timecode:` used to print an elapsed-time conversion of the frame index for every
file — ignoring the real start timecode on files that carry one and inventing `00:00:00:00`
for files that carry none, both of which the spec forbids. `frameToTimecode` is renamed
**`frameToElapsed`**, which is what it always computed, and the readout is four modes:
`F` Frame Count, `S` Seconds, `E` Elapsed, `T` **source** SMPTE. Also: `Ctrl+G` Go to Frame
and `Ctrl+Shift+G` Go to Timecode, a Time Display menu, and the image-sequence and still HUD
lines finally zero-based (they printed `currentFrame + 1` against a *count*).

Four things to carry.

- **`hasSourceTimecode_` is the single gate**, asked by the readout mode, the menu item and
  Go to Timecode alike, so "this file has no timecode" cannot be true in one place and false
  in another. `setReadoutMode` **declines** SMPTE with a reason rather than accepting it and
  rendering something else — `T` on an MP4 reads `Timecode: source carries none`. Opening
  media without a timecode while SMPTE is selected resets to Elapsed, which is the case the
  gate cannot catch because nothing was selected: the file changed under a mode already set.
- **Extraction reads three dictionaries and never synthesises**, and the value is parsed and
  re-formatted rather than stored raw, so anything unreadable becomes "no timecode" inside the
  decoder rather than reaching a readout that would print it verbatim and call it SMPTE.
  `TRACE_OPEN_LOG` gained a `timecode=` column that prints **`none`**, not a blank.
- **DROP-FRAME HAD NO TEST MATERIAL, SO THE MATERIAL WAS MADE**
  (`scripts/measure/make_timecode_fixtures.ps1`). The asset set is 24/23.976/60fps throughout
  and every timecode in it is non-drop, so shipping DF arithmetic would have been §29.2 again.
  **The first fixture pair could not have failed**: starting at `00:59:50` crosses minute 60,
  a multiple of ten, where drop-frame skips nothing and both conventions print identical
  digits. Starting at `00:00:50` puts a dropping minute inside the clip, and at the same frame
  index the two now read **`00:01:00;02` against `00:01:00:00`** — that difference is the
  proof the DF path runs rather than compiles. On real media, ProRes 4444 with a start of
  `00:00:01:12` reads it at frame 0 and `00:00:02:00` twelve steps later.
- **The shortcut guard finally had something to guard, and it holds.** Five phase records said
  `ShortcutTable`'s key-only matching made a text field dangerous and that it was untestable
  because there was nothing to type into. Measured: with Go to Timecode open, typing
  `hjkltefsm` — every bound single-key command — puts **`hjkltefsm` in the field** and changes
  nothing behind it. Two Qt mechanisms do it (`QEvent::ShortcutOverride` on `QLineEdit`, and a
  modal dialog being a separate window), neither of which needed writing and neither of which
  had ever executed. **A new single-key shortcut still has to be checked against this** — the
  guard is Qt's, not Trace's, and covers *printable* keys only.

Both Go To prompts **validate before seeking and refuse rather than clamp** — a clamped
mistype would move the playhead somewhere the user did not ask for and look like it worked —
and both land through one shared exact `Step` seek, so neither needed decoder work.

Regression against a control built from `19f9383`, hash-verified, same display: 4K H.264
cadence ×3 99.1–99.2% with identical buckets, 4444 99.8% ×2, reverse 1× 100.0% ×3,
`-SnapRelease` `delta 0` / `hitch 0`, both lifecycle legs, **25 of 25 transitions** on both
binaries.

**SPEC PHASE 8 IS DONE (2026-08-11): the Share menu ships, and its third command is present,
visible and unable to run.** Copy File Path and Show in File Explorer work; **Copy LucidLink
Link is the *gate* only** — the link itself is phase 9, and the spec forbids combining
uncertain LucidLink shell work with otherwise safe visual changes. Three `QAction`s and
**one** `QMenu`, reached from the menu bar (File ▸ Share), the docked bar's Share button and
the composited overlay's new Share region. Gate and shell calls live in `src/app/MediaShare.*`.

Five things to carry.

- **The classifier is a NECESSARY condition and can only ever say no.**
  `MediaIoSource::classifyStorage` is reused rather than rewritten, but it answers a
  *storage-class* question — "virtual mount, petabyte capacity, `free == total`" — which is
  true of any such mount. So in `evaluateShare` it can only move the verdict from
  **Unavailable** to **Disabled**, never to Available; the third condition, the installed
  integration, is the only thing that could. `lucidLinkIntegrationAvailable()` returns false
  with a reason saying only what has been established — *this build has no integration* —
  rather than the design package's "LucidLink is not running", which asserts a cause nothing
  has checked. Phase 9 replaces the body and the string together.
- **Disabled and Unavailable are kept distinct and neither row is ever hidden**, per the
  design package §9. A local file reads Unavailable; a virtual mount with no integration reads
  Disabled. `copyLucidLinkAction_` has **no handler connected at all** — an action that appears
  to exist and changes nothing is the `showInfo` failure phase 2 deleted.
- **VERIFYING A GREYED MENU ITEM FROM A SCREENSHOT DOES NOT WORK, and it accused a correct
  build.** Peak label luminance read **230 on all three rows**; menu-icon luminance read
  227/202/247, which cannot separate a disabled row from a shorter label with a different
  glyph. The gate went into the HUD instead, on the storage line beside the classification it
  is built from: **`share path ok explorer ok lucid unavailable`** on a local file, and
  **`lucid disabled`** under `TRACE_REMOTE_IO=1`. **That second reading is the negative
  control** — both branches are live, they differ, and *neither says `ok`*.
- **The Share button fits INSIDE the settled 460×84 panel**, because the three centred controls
  only reach 78 logical px either side of centre and the approved package puts share at the
  right of that row anyway. Phase 6's numbers are untouched. One thing had to move and it was a
  real overlap: the **rate-flash chip is top-LEFT now** — at 84px of panel height a top-right
  chip spans y 10–31 against a 34px control's 13–47. (The package actually specifies the chip
  *centred above* the transport, §6; still unimplemented, not this phase's to change.)
- **"File removed while open" took two attempts and the first accused the code.** Windows
  refuses to delete a video file Trace has open, so the obvious test cannot run. A **directory
  junction** was tried next and Qt still resolved the path after the junction was deleted, so
  the HUD read `explorer ok` and it looked like a gate bug. It is not: a **still image** is the
  case where Trace does not hold the handle, and deleting one while displayed greys Show in
  File Explorer exactly as intended.

Show in File Explorer goes through `SHOpenFolderAndSelectItems` on a `QThreadPool` task, not
`explorer.exe /select,<path>` on the UI thread: there is no process-argument quoting of that
command line that works for every path, and the spec forbids blocking shell calls on the UI
thread. **A real `V:\` LucidLink path was NOT tested** — it is live client storage and no file
was nominated — so the virtual-mount branch was exercised through `TRACE_REMOTE_IO`. Phase 9
needs a nominated file.

**SPEC PHASE 9 IS DONE (2026-08-11): Copy LucidLink Link works, and Trace never composes a
link.** The gate's third condition is now answered by the **installed integration, for the
specific file**, and the link itself is produced by that integration. Code in
`src/app/LucidLinkIntegration.*`.

Six things to carry.

- **THE DAEMON'S REST API IS AUTHORITATIVE AND IS STILL NOT THE MECHANISM.** LucidLink runs a
  local REST service (the CLI's own `--rest-endpoint`), and `GET /fsEntry?path=...` returns
  **`"id" : "2955:105901"`** for the nominated file — exactly the identifier in the expected
  link. But **no endpoint returns a link**. Assembling
  `lucid://<filespace>/file/<id>/<name>?reveal=true` from the parts is hard-coding LucidLink's
  URL format, which the requirement forbids, and newer installations may emit an
  `app.lucidlink.com` HTTPS link instead. The vendor's own extension does that assembly
  internally — `LucidShellExt.dll` carries `lucid://`, `/file/`, `?reveal=true` and
  `/fsEntry?path=` as literals — which is the point: **the format is theirs.** The REST API
  remains the right tool for *validating* an id.
- **ONLY THE LUCID HANDLER IS CREATED, AND THAT IS A SAFETY PROPERTY.** Building the merged
  Explorer context menu would load every registered handler into Trace's process (Adobe,
  OneDrive, PowerToys, Tailscale, Copilot on this box). `CoCreateInstance` on the one
  discovered CLSID → `IShellExtInit::Initialize` with the file's `IDataObject` →
  `QueryContextMenu` on a private popup gives a menu of **only LucidLink's commands**. That
  matters because the item beside the wanted one is **`Pin`, which hydrates the file onto the
  mount**, and `V:\` is live client production storage. Identification is an **exact match on
  the display text, never positional**, and a miss reports unavailable rather than falling
  back to anything.
- **The extension exposes NO canonical verb** — `GetCommandString(GCS_VERBW)` fails for every
  item it contributes — so the display string is all there is. Measured against
  **LucidShellExt 1.0.15**, which renders `Copy link`. A localized Windows would render
  something else and Trace would report the integration unavailable rather than invoke the
  wrong item. **Failing closed is deliberate.** CLSIDs are discovered from the registry rather
  than hard-coded, and both installed generations are tried.
- **The classifier is still only a necessary condition.** What supplies Available is the
  extension's own answer for the file: outside a linked filespace its `Initialize` returns
  **E_INVALIDARG** and it offers nothing. Three states, all measured — local file
  **`lucid unavailable`** (and **no probe is started at all**, so no COM and no third-party
  DLL is loaded for local media); local file under `TRACE_REMOTE_IO=1`, i.e. eligible but
  declined, **`lucid disabled`**; nominated file **`lucid ok`**. The middle row is the
  requirement's own negative control and is a real path rather than a simulated one.
- **The link is exact.** Driven from the overlay's Share menu: `InvokeCommand -> 0x00000000`,
  clipboard accepted after **21ms**, and a **case-sensitive** comparison against
  `8_LucidLink\LucidLink.txt` matches. The clipboard is snapshotted, the change waited for by
  `GetClipboardSequenceNumber` with a 4s timeout, and the result validated as a supported form
  (`lucid://` or `https://app.lucidlink.com/`) — anything else is rejected and the old value
  restored. Only `CF_UNICODETEXT` is snapshotted, so a clipboard holding an image cannot be
  restored; that is stated rather than hidden.
- **THE INSTRUMENT WAS THE BUG AND IT NEARLY BECAME A MECHANISM.** The first build read
  `lucid disabled`; switching the worker's apartment from `CoInitializeEx` to `OleInitialize`
  made it read `lucid ok`, and "a shell extension needs the full OLE stack" was about to be
  written down as the fix. **It is wrong** — that build also failed to `refreshHud()` after the
  probe landed, and a paused file does not refresh, so the HUD was showing open-time state
  while the *menu* had been correct all along. `TRACE_LUCID_COINIT=1` is the retained control:
  **both apartments read `Initialize 0x00000000` and `SUPPORTED (offset 2)`**. `OleInitialize`
  is kept as a precaution, not as a fix. **Second time in two phases that a stale instrument
  accused a correct build** — phase 8's was menu-icon luminance.

**The 1×1 and 4×5 ProRes assets are in the set** (`9_1x1_ProRes`, `10_4x5_ProRes`): 23.976
ProRes 10-bit, 528 frames, both carrying a **non-drop start timecode of `00:59:53:00`** which
is read from the container and honoured — frame 0 reads it and frame 24 reads `00:59:54:00`,
which is exactly one timecode second. **CPU and D3D11 framing agree exactly** on the 4×5
(`display 288x360`, `win 1280x767` on both). **One carried defect, not fixed by instruction:
the floating transport is 460 logical px wide against a 288px video rect on the 4×5**, so the
panel is 1.6× wider than the picture and covers much more of a 1×1 or 4×5 image than of a
16:9 one. Owner visual-review item; the approved package's §8 media-shaped window would change
the premise entirely.

**SPEC PHASE 10 IS DONE (2026-08-11): the view transforms are wired, and rotation rotates what
the user SEES.** Five shared `QAction`s in a real **Edit** menu. Wiring only — the
renderer-neutral contract was built and measured at plan §31 and neither backend needed a
line. `TRACE_VIEW_TRANSFORM` **left with the phase**, the way `TRACE_SHUTTLE_ENTRY` did at
phase 5.

Five things to carry.

- **ROTATION GOES THROUGH `rotatedOnScreen()`, NOT `quarterTurns + 1`, and that is the whole
  combined-rotate-and-flip determinism question.** The composition is
  `screen = flip(rotate(source))`, so the *flip* buttons already act on what is visible — but
  **a mirror reverses the sense of a rotation applied after it** (`R(t)·M == M·R(-t)`). With
  exactly one mirror in force, Rotate Right must **decrement** `quarterTurns` or the picture
  visibly turns **left**; with both it must not, because H then V is a 180° rotation and
  rotations commute. It lives on `ViewTransform` so both backends inherit one answer. Flips
  need no compensation and are plain toggles.
- **Verified by a landmark, not by the arithmetic.** The 4×5 slate's black bar is bottom-right
  at identity, bottom-left after Flip Horizontal, and **top-left** after Rotate Right — where a
  clockwise turn puts it. State reads `view rot270 flipH`; the naive version reads `rot90 flipH`.
- **The fit and the reduction taps come from the POST-TRANSFORM fit**, measured across the full
  cycle on 4K H.264: identity `640x360 filtered x3`, rot90 `202x360 filtered x4`, rot180
  `640x360 filtered x3`, rot270 `202x360 filtered x4`, and four presses return to identity with
  the `view` field gone. Those are §31's predicted values **to the digit**. 180° changing
  neither is the check that the taps track the *fit* rather than the rotation. The 1×1 stays
  `360x360` under rotation, which is the degenerate case worth having.
- **`repaint()`, NOT `update()`, when a transform is applied — the HUD was reporting the
  PREVIOUS transform.** The fit and the taps are measured *by* the paint and reported after it,
  so refreshing after a merely-scheduled repaint prints the old `display`, and a paused file
  never refreshes again. Measured: the 4×5 rotated 90° drew visibly landscape while `display`
  still read `288x360`. **Third stale-instrument finding in three phases** — phase 8's
  menu-icon luminance, phase 9's un-refreshed HUD after the LucidLink probe, and this. In all
  three the code was right and the instrument accused it.
- **CPU and D3D11 agree on orientation, fit and framing**, which the plan warned might not hold
  because QPainter post-multiplies. `display` and `win` are identical on both at rot90,
  rot90+flipH and flipV. Band diff (docked bar, `scripts/measure/banddiff.ps1`): identity 0.79%
  / max 141, rot90 1.04% / 154, **rot90+flipH 1.04% / 154 — identical to rot90 to the pixel**,
  which is what an *exact* mirror on both backends predicts, since flipping both captures maps
  the difference map onto its mirror. A mirror *disagreement* would have read near 50%. **The
  first attempt read 9.1% and was the floating overlay's fade state landing inside the band —
  a cross-backend diff has to be taken in bar mode.**

**The transform is viewing state and survives the transport**: `view rot90` is present through
playing, paused, stepped, shuttle, stopped, scrubbed, fullscreen and back, with the frame index
advancing normally underneath it (0 → 60 → 157) and no decoder request made. **Frame numbering,
source timecode and the share gate are untouched** — with `rot90` on the 1×1, `Timecode:` reads
`00:59:53:00` at frame 0 and `00:59:54:00` at frame 24, the same as untransformed. **Reset
works both ways**: the action returns to identity, and opening a different file resets it.

**Reset View Transform has NO shortcut on purpose.** The approved package puts it on `Ctrl+0`
and the interface spec gives `Ctrl+0` to Actual Size; the spec governs, its conflict rule is to
preserve the existing binding, and Actual Size does not exist yet — so this phase claims
neither. `Ctrl+L`/`Ctrl+R` are unclaimed in both and are taken. The menu item is **"Rese&t"**,
not "&Reset", because Rotate Right already owns R there and two items sharing a mnemonic makes
the key cycle the highlight instead of activating either.

**SPEC PHASE 11 IS DONE (2026-08-11): Open Recent ships, and Trace has a settings home.**
`File ▸ Open Recent`, bounded at 10, with Clear Recent Files. `src/app/Settings.*` and
`src/app/RecentFiles.*`.

**THE SETTINGS HOME IS AN OWNER DECISION, NOT QT'S DEFAULT** (owner, 2026-08-11): a
`trace.ini` **beside `Trace.exe` when one exists and is writable**, otherwise
`QSettings::IniFormat` under `AppConfigLocation`. Never `NativeFormat` — that writes
`HKCU\Software\<org>\<app>`, and a portable ZIP with no installer must not leave registry keys
behind after its folder is deleted. Trace **never creates** the portable file: its presence is
how a user asks for portable mode, and creating it would make every installation portable.
A read-only one **falls back and says so on stderr**. All three branches were run and differ
(`TRACE_SETTINGS_LOG=1`). **`trace::app::settings()` is the one home and must not grow a
second** — phase 6's fullscreen geometry, phase 14's window state and §4's aspect lock all
want it. `QSettings` still appears in `LucidLinkIntegration.cpp`, but that is registry
*reading* for CLSID discovery, not a settings home.

Five things to carry.

- **THE REFUSALS ARE ENFORCED BY MAKING THEM IMPOSSIBLE.** `RecentFiles.cpp` has **no
  `QFile`, `QFileInfo` or `QDir` in it at all**, and `rebuildRecentMenu()` takes a basename by
  searching the string, because `QFileInfo` is precisely the call that must not be there. The
  menu is drawn from stored strings, **every row is always enabled**, and the submenu is
  rebuilt when the list *changes* rather than on `aboutToShow` — identical cost today, but
  `aboutToShow` is the natural home for a later "just check quickly".
- **THE CONTROL IS 21 SECONDS LONG, AND WITHOUT IT THE CHECK COULD NOT FAIL.** An unreachable
  UNC path costs **21,037ms** to stat on this box (two *different* hosts, because Windows
  caches a failed lookup for ~10s). Ten seeded entries, two of them such paths: window up in
  **708ms against 752ms on an empty list**, i.e. a 42-second budget unspent. **And the HUD's
  `recent 10/10` is what says the seeded list was actually read** — without it the poison leg
  would have been the clean leg run twice.
- **NO PROBE BEFORE THE OPEN EITHER.** "Check it is there, then open it" pays the disconnected
  mount's cost twice. `openRecentPath` hands the path straight to `openPath` (which returns
  **bool** now), so the recent list never makes Trace touch a path the user did not just ask
  for; existence is asked only **after** a failure, when it is free. That distinction is load
  bearing: **"the open failed" and "the file is gone" are different conditions**, and only the
  second may offer to remove the entry. A 4KB file of garbage named `.mp4` produces no prompt
  and no recent entry.
- **Both buttons of the missing-file prompt were pressed.** Remove takes the list 10 → 9 and
  clears the stored row; **Keep leaves it at 10**. An offer that removes the entry whichever
  button is pressed is not an offer. Keep is the default so a stray Return is not destructive.
- **`MediaShare::canonicalNativePath` left its anonymous namespace rather than being written
  twice**, and it costs nothing extra because the Share gate canonicalises the path a few
  lines earlier in the same open. **The `&` in `M&M_TopGun_1080.mp4` is escaped** — unescaped,
  Qt draws `MM_TopGun_1080.mp4` and silently claims Alt+M; that filename is the only one in
  the asset set that catches it.

Regression against a control built from `1207837`, hash-verified (`3DC518E0` / `3CD91CF2`),
**physical panel 5120x1440 @ 239.999Hz**, `win 1280x843`, `display 640x360 1:1`: 4K H.264
cadence ×4 100.0% with identical buckets, 4444 ×3 99.8%, `-SnapRelease` `target 120 shown 120
delta 0` full-res planar / `hitch 0`, reversal drag `hitch 1` / `delta 0`, both lifecycle legs,
**25 of 25 transitions**, `paints` unchanged. **Launch to window was A/B'd because this is the
first phase to read a file in `MainWindow`'s constructor**: min 701 / med 704ms against the
control's 710 / 722. Reverse 1× went bimodal into the recorded populations on both binaries —
the first three-run pass read 3 of 3 slow against 1 of 3 and looked like a regression; five
more each settled it at **3 of 8 against 5 of 8**. One transitions case FAILed once with "no
window after restart" and re-ran 3 of 3 PASS.

**SPEC PHASE 12 IS BUILT AND MEASURED (2026-08-11): the window is the shape of the media.**
Spec §4. It had **no phase number** — the spec's own phasing list stops at 14 and §4 was
appended after the main body — and the owner scheduled it ahead of the Movie Inspector because
the inspector reports *current viewport size*. Everything after it shifts by one: Movie
Inspector 13, menus/help/accessibility 14, full regression 15.

**§2 ITEM 7'S PREDICTED CACHE THRASH DOES NOT EXIST, and that was the phase's first
experiment** (`scripts/measure/resizecache.ps1`). A real corner drag produces ~123 resize
events and ~122 real preview-size changes, and **exactly one of them discards anything** —
the drag throws away precisely the number of entries the cache held (1, 32, 7 and 68 on four
legs), because clearing an empty cache is free and **nothing refills it while the pointer is
down**. So deferring the clear to `WM_EXITSIZEMOVE` saves nothing; it moves one clear from the
start of a drag to the end. `syncScrubPreviewSize` costs **0.2–0.3ms across a whole drag**,
0.02ms worst event. Item 7's *other* cost is misdescribed too: `reclaimDecoder()` returns at
its first line when `!decoderLeased_`, and **no lease is out during a resize** — a resize drag
and a scrub drag cannot be the same gesture — so the "unconditional generation bump per event"
never happens. **Eighth premise-expiry, and the first where the item had already been
re-derived once**: §2 item 7 is the 2026-08-10 correction of the 2026-08-09 text, and it fixed
the mechanism while keeping the conclusion. The conclusion was the wrong half.

**THE THREE WIN32 MESSAGES ARRIVE EXACTLY AS ASSUMED, and `nativeEvent` had never run in this
project.** Every drag: **1 `WM_ENTERSIZEMOVE`, 121–126 `WM_SIZING`, 1 `WM_EXITSIZEMOVE`**, with
`WM_SIZE` matching Qt's `resizeEvent` count to the digit. `WM_SIZE` is counted as the **control
on the other three** and earned its place immediately — see the stale instrument below.

**FIFTH STALE INSTRUMENT, AND THIS ONE IS IN SHIPPING CODE.** `refreshHud()` is not called on
`resizeEvent`, so **a paused window that is resized redraws the HUD at the new size with the
old string in it** — `win`, `display` and every counter a phase quotes. The first run of the
experiment read `resize 1 … wm 0/0/0` while its own capture was 200px narrower than the shot
before it, which looks exactly like a gesture that missed the resize border; `WM_NCHITTEST` at
the grabbed point returns **17 (`HTBOTTOMRIGHT`)**, so it had worked all along. Not fixed in
`resizeEvent`, because `display` is measured **by** the paint (phase 10) and building the HUD
string on 123 events per drag would put the instrument inside the path. The harness refreshes
through a short play run *after* the drag instead.

**THE SOURCE'S SHAPE IS READ AND NEVER ASSUMED.** `av_guess_sample_aspect_ratio` (not
`codecpar->sample_aspect_ratio`) composes the codec's SAR with the container's and with any
container DAR — **that composition IS the spec's "DAR metadata when authoritative"**, so there
is deliberately no second DAR field to disagree with it. Rotation comes from the display matrix
through `av_display_rotation_get`, converted to clockwise, snapped to a quarter turn, **and the
snapping is reported**. `sarStated` is separate from the value for the same reason
`colorMatrixInferred` is, and it has a **real negative control in the shipping assets** —
three of four real files state 1:1, the 9:16 clip states nothing.

**THE ASSET SET IS ENTIRELY SQUARE-PIXEL AND UNROTATED, so the fixtures were made**
(`scripts/measure/make_shape_fixtures.ps1`): 1440x1080 SAR 4:3, 1920x816 SAR 6:5, and a rotated
pair. **`rotated-180` is the fixture that matters** — 180° leaves the ratio alone, so it is the
only one that fails a build checking `rotation != 0` instead of asking *which* rotation.
Without it, "rotation is handled" would be provable by a build that transposes on all of them.
Trace agrees with ffprobe on all four. Note **`-display_rotation` is an INPUT option**: written
as an output option ffmpeg accepts it and produces no file, which is how the first version
failed silently.

**THE PICTURE HONOURS THE SHAPE TOO, and it had to.** Sizing the window to the display ratio
while the picture is still fitted on stored dimensions just pillarboxes inside it, and §4's
"the image touches all four viewport edges" is then unsatisfiable. One line per backend plus a
shared `applyPixelAspect()`. **Container rotation is composed with the user's transform in ONE
place** (`ViewerWidget::applySourceShape`) rather than sent to a backend separately — which is
what makes **Reset View Transform mean "back to how the file says it should look"** rather than
"back to un-rotated". Verified: `rotated-90` plus one `Ctrl+R` draws the frame exactly as
encoded while the HUD reports `rot270` and `view rot90` separately. **The reduction taps take
the source size WITHOUT the pixel-aspect stretch** — the opposite correction to phase 10's,
because SAR adds no texels while rotation exchanges real texel axes.

**THE CONSTRAINT IS APPLIED IN `WM_SIZING`, NEVER CORRECTED IN `resizeEvent`.** §4's three
requirements — dragged edge authoritative, other dimension follows smoothly, no recursion or
oscillation — are one requirement with one answer: correcting afterwards is what *produces*
oscillation, because Qt has already laid out a wrong-shaped window and the correction is itself
a resize. Only the edges the user is not dragging are moved. Measured: **the right edge tracks
the cursor to the pixel** through a five-step drag while height follows width at 40:50 on 0.8
media. A corner's authoritative axis is decided against the rect at `WM_ENTERSIZEMOVE`, not
against the previous proposal, or it can change its mind mid-drag.

Two faults the first cut had, **neither visible in `win WxH`**. **`setGeometry` on a top-level
widget positions the CLIENT rect**, so centring it pushed the title bar off the work area by
exactly −7px on every shape — a whole title bar, reading as a rounding error. And **chrome
measured as window-minus-viewer is only the chrome while the layout can satisfy everything**:
at open the viewer is pinned at its own floor, so it read **310 against a real 407** and
pillarboxed the 4×5 inside a window built to have no bars. It **converges in at most two
passes** now, measuring what the layout did rather than predicting it — which also avoids
hand-listing menu + status + HUD + transport bar, a list phase 6 would already have broken.
Final viewer aspects: **1.7781, 0.8002, 1.0000, 0.5629, 2.8235**.

**`ViewerWidget`'s fixed 640x360 floor was itself a 16:9 assumption** — a 9:16 clip could not
go below 640x1138. It is **360 logical px on the shorter displayed axis** now, which is
640x360 at 16:9 **to the pixel**, so no 16:9 startup geometry moves.

**Snap needs no detection**: it resizes through `SetWindowPos` and sends no `WM_SIZING`, so
"never fight Windows" is automatic. Returning to normal reapplies the lock **only when the
restored geometry is the wrong shape**, because §4 asks in one paragraph both to restore the
previous position and to reapply the lock, and reshaping recentres.

**Regression (physical panel, 5120x1440 @ 239.999Hz):** 4K H.264 cadence ×3 **99.1/99.1/99.2%**
against a lock-off control on the same binary at **99.2/99.1/99.2%** with identical buckets —
flat; 4444 ×2 **99.8%** with `handler>budget 0 of 260`; `-SnapRelease` `delta 0` / `hitch 0`;
both lifecycle legs; **25 of 25 transitions**. **The reversal drag moved and is attributed
rather than excused**: the shaped window gives 4K H.264 `display 1474x830` against `640x360`,
5.6× the area, so `cache 215 → 77`, `rev-hit 98.7 → 96.7%`, **`hitch 1 → 2`** — §22.8's
window-size effect with the lock as the only difference between the two runs. **Every scrub
baseline recorded before phase 12 was taken in a much smaller window and is not comparable to a
default-size run today.**

**THE OPENING WINDOW IS CAPPED — OWNER DECISION, 2026-08-11, and it AMENDS §4 rather than
implementing it.** Media determines the opening window's *aspect ratio*, not an unlimited
source-pixel-sized window; 4K must not open enormous. Natural size only when already small; a
**1280x720-equivalent logical-pixel AREA** cap reshaped to the media's aspect; never past **80%
of the work area** including chrome; the **settled 460px transport** sets a floor that very
small media is enlarged to meet; **one proportional scale**, never a per-axis clamp. **The cap
is an AREA and that is what makes it shape-neutral** — capping a width would give a 9:16 clip a
quarter the window of a 16:9 one. At equal area: 16:9 → 1280x720, 1:1 → 960x960, 4:5 →
859x1073, 9:16 → 720x1280. Precedence where they disagree: the transport minimum may push past
the 80% budget, never past the work area itself.

**It did what it was taken for.** 4K H.264 reversal drag: `display 1474x830 → 1066x600`,
`cache 77 → 141`, `rev-hit 96.7 → 98.2%`, **`hitch 2 → 1`** — back to the lock-off control's
figure while keeping the media-shaped window. Regression after: cadence ×3 99.1/99.2/99.2% with
`handler>budget 0 of 120`, 4444 ×2 99.8% at 0 of 260, `-SnapRelease` `delta 0` / `hitch 0`,
both lifecycle legs, **25 of 25 transitions**.

**`src/app/WindowShape.cpp` IS SEPARATE FROM `MainWindow` BECAUSE THIS BOX CANNOT TEST DPI.**
§4's matrix names 100/125/150/200% and this machine runs at **100%**, so every
`devicePixelRatioF()` term is the identity on the only box that can drive the app by hand.
`computeViewerSize()` takes `dpr` as an **argument**, and `Trace.exe --window-shape-selftest`
drives **11 shapes × 4 scale factors** with no window, renderer or display — now a CI step, so
it runs on every push. The shipping path calls the same function, so it is not a second
implementation that agrees today.

**THE SELFTEST'S FIRST INVARIANT WAS WRONG AND FAILED SEVEN ROWS ON CORRECT CODE.** "The same
logical size at every scale factor" is false when natural size binds, because **natural
displayed size is a PHYSICAL statement** — a 1920-wide source is 960 logical px at 200%. Which
quantity is invariant depends on **which rule bound the result**, which is why `ShapeBound` is
reported rather than inferred: `logical × dpr` for natural-bound rows, the logical size alone
for cap/work/minimum rows. A build that multiplies where it should divide fails both halves.
44 rows pass, and the real 1.00 path on this machine matches the selftest for the same inputs.

**SYNTHETIC DPR IS NOT MIXED-MONITOR VALIDATION AND MUST NEVER BE QUOTED AS SUCH — and the
2026-08-14 hardware pass is what turned that caution into a demonstrated fact.** Real
`WM_DPICHANGED`, swapchain resize and monitor-to-monitor moves were **UNTESTED** for want of a
second display (§20.4) until 2026-08-14. **All 44 rows here passed on a build where the
shipping path was broken**: §4's sizing never re-ran on a DPI change, so a window crossing a
100% → 150% boundary came off it the wrong shape with the picture pillarboxed, and this
selftest could not have seen it. **A pure function cannot notice that nobody called it.** The
selftest still prints its caveat on its own last line, narrowed to what it actually proves —
the arithmetic — and pointing at `scripts/measure/dpimove.ps1` for the hardware case.

**PHASE 12 IS SIGNED OFF (owner, 2026-08-11) and nothing about the media-shaped window is
open.** Media-shaped windows look correct across landscape, square, portrait and narrow media;
the 4K opening size feels appropriate *on the capped policy*; aspect-locked resizing is stable
and unlocked resizing works; maximized, snapped and fullscreen are correct; rotation updates
the fitted orientation; and **stills and image sequences use the same correct sizing path** —
which closes the phase's one remaining measurement gap by observation, because that path was
built to be the same path.

**THAT LAST CLAUSE WAS WRONG AND HAS BEEN RE-SIGNED-OFF ON A CORRECTED BUILD (owner,
2026-08-11; fix at `3a38516`, re-sign-off recorded in the phase 13 block below).** §4 had
**never applied to a still or an image sequence at all** — `LoadedImageInfo::image` is left
default-constructed at both sites that build one, so `currentDisplayAspect()` read an empty
`QSize` and declined. The path really was the same path; **its input was empty**, and the
distinction is the whole lesson. The 4096×2304 still opened at ratio **1.896 against the
file's 1.7778**, pillarboxed inside a window built to have no bars; it is **1280×720 exactly**
now. **The video half of this sign-off is untouched and did not need replacing** — only the
still and image-sequence clause did.

**TWO CARRIED ITEMS CLOSE WITH IT.** The **narrow-media transport-width item is CLOSED** — the
460px panel on 1×1, 4×5 and 9×16 media is *tight but usable and visually acceptable*. It had
been carried since phase 9, when the panel was 460px against a **288px** picture, and §4's
media-shaped window changed the premise rather than needing a panel fix, exactly as predicted.
And **the clipped dev HUD on narrow windows is a DIAGNOSTIC LIMITATION, not a product defect**
(owner) — it stays a measurement hazard to check for, and it is not work.

Read it at its stated width: this sign-off was taken **single-display**, and a **geometry**
sign-off is display-dependent in a way a colour one is not — the work area is an input to the
opening size, so Parsec's 1920x1200 would bind the 80% rule much harder and give visibly
smaller windows for the same media. The machine reported the physical panel when this was
written; establish the display first if the shaped window is ever questioned.

**Mixed-monitor DPI was validated on 2026-08-14 and it FOUND A DEFECT IN THIS SIGN-OFF'S
SUBJECT** (§20.4 below): §4's sizing pass never re-ran when the window crossed a scale-factor
boundary, so a media-shaped window came off the crossing the wrong shape with the picture
pillarboxed inside it. Fixed at `8945894`. **The sign-off is not withdrawn** — it was taken on
one display and everything it covers still holds there — but "maximized, snapped and fullscreen
are correct" now also holds across two monitors at 100% and 150%, which is more than was
observed at the time.

**SPEC PHASE 13 IS DONE (2026-08-11, `368e3b8`): the Movie Inspector ships, and every row
says where its value came from.** The metadata layer landed at `9ec7ec3`; this is the window
over it. Modeless, collapsible, `Ctrl+I`, in a new **Window** menu — where the spec puts
Show/Hide Movie Inspector literally. Code in `src/app/MovieInspector.*`.

Six things to carry.

- **EVERY ROW CARRIES ITS ORIGIN, AND THAT IS THE SPEC'S SECOND REQUIREMENT RATHER THAN
  DECORATION.** The metadata layer answered *"distinguish encoded metadata from playback
  inference"* for the four colour tags; the same line runs through the whole panel. Four tags:
  **`encoded`** (what the file states), **`file`** (the file on disk), **`observed`** (this
  window now — viewport size, current scale, orientation on screen), **`playback`** (what Trace
  did about it). Measured on the 2–2 split: `Splash_1.mp4` reads **`Untagged` on all four
  colour rows** with **`Playback is using: bt709 matrix (inferred by Trace — the file states
  none)`** directly beneath, and 4444 reads `bt709` ×3 / `Limited` with `(as tagged)`.
- **THE METADATA LAYER DID NOT CARRY PIXEL FORMAT OR BIT DEPTH, AND BOTH OBVIOUS SOURCES ARE
  WRONG FOR AN INSPECTOR.** `VideoPerfStats::srcPixelFormat` is rewritten by every conversion
  and gains `" (a-skip)"` once alpha is dropped — it is what playback last *did*. And
  `srcBitDepth` is `av_get_bits_per_pixel()`, i.e. bits per **pixel**: it reads **12 on 8-bit
  yuv420p and 48 on 4444**, so a "Bit depth" row built from it tells the user an 8-bit H.264
  file is 12-bit. `VideoMetadata` gained `pixelFormatName`, `bitsPerComponent` and
  `bitsPerPixel`, read once at open. **ffprobe agrees on three files: 8 / 12 / 10.** Both are
  now on screen together on 4444 — `yuva444p12le` under `encoded`, `yuva444p12le (a-skip) →
  YUV444P12 planar` under `playback`.
- **THE DIALOG READS AND CANNOT ASK.** `MovieInspector.cpp` contains **no `QFile`, `QFileInfo`
  or `QDir`, no decoder and no viewer** — it takes a value type. The handoff predicted this
  file could not follow `RecentFiles.cpp`'s rule *"since it must report a size"*; it can,
  because the size is not computed there. Video takes it from `VideoPerfStats::sourceBytes`
  (read by `MediaIoSource` while opening the file); a still takes it from the **one `QFileInfo`
  `openPath` already built to read the extension**. Nothing stats a path when the window is
  shown — 21,037ms on an unreachable UNC host.
- **THE REFRESH IS A 150ms COALESCING SINGLE-SHOT, WHICH IS PHASE 10's TRAP AND NOT A
  DEBOUNCE.** `lastDrawSize` is measured *by* the paint, so a refresh issued where the change
  happens reports the previous viewport and a paused file never corrects it. Armed by media
  open, view transform and resize; **never armed while the window is hidden**, so "do not
  continuously poll" holds by construction, and a corner drag's ~123 resize events collapse
  into one rebuild. Cross-checked after a resize: HUD `display 643x362`, inspector
  `643 × 362 px`.
- **A MODELESS WINDOW MUST NOT HOLD THE FLOATING TRANSPORT REVEALED, AND THE CONTROL SAYS THE
  ACCIDENT WAS REAL.** `QApplication::focusWidget()` is application-wide, so a separate
  top-level window satisfies `holdVisible`'s child-focus branch for as long as it is focused.
  Scoped to **`focus->window() == this`**. Measured: hidden → revealed changes **4.24%** of the
  video band (a 460×84 panel is ~4.3% of it), and with the inspector focused for 4.5s the band
  reads **0.07% from hidden, 4.32% from revealed**. **A control with that one clause reverted
  swaps the two exactly.** The modal branch is untouched, so both Go To prompts still hold.
- **`Ctrl+I` IS A `QAction`, NOT A `ShortcutTable` DISPATCH ROW** — phase 3's rule, since that
  dispatcher ignores modifiers and would have opened the inspector on plain `I`. It is in the
  table as a documentation row so phase 14's Keyboard Shortcuts window stays complete. **Plain
  `I` is not resurrected.**

Two layout faults, both found by looking at the window. A source path is one unbroken token, so
a wrapping `QLabel` holding it demanded a very wide minimum and **pushed the origin column off
screen** — on the one window whose purpose is to say which claim is which. Constraining the
label instead cost it height-for-width and **clipped the path to `C:`**. The path gets a
read-only entry; every other value keeps a wrapping label, because every other value has spaces
in it.

**`scripts/measure/inspector.ps1` is new** (`show` / `viewport` / `hold` / `media`). Two harness
faults worth carrying: **`$mn[0] + 30` on the strings `-split` returns is CONCATENATION** in
PowerShell, so every pointer coordinate landed off-screen, the run read 0% changed with the HUD
showing `+overlay`, and **it accused the app for three runs**; and the first `hold` leg took its
baseline before `Ctrl+I` and read 39% changed, which was the inspector *window* appearing over
the transport rather than the panel fading — it would have passed a build that held the
transport up forever. `-Mode media` opens the second file through File ▸ Open **in the same
process**, because a second launch tests nothing. `-Mode hold`'s second leg **reports NOT RUN**
rather than a number: Windows refuses `SetForegroundWindow` to a background process.

**A PHASE 12 DEFECT CLOSED WITH IT (`3a38516`): the media-shaped window had never applied to
stills or image sequences.** `LoadedImageInfo::image` is left default-constructed at both sites
that build one, so `currentImage_->image.size()` is an **empty `QSize`**,
`currentDisplayAspect()` returned 0.0 at its `isEmpty()` test, and §4 silently did nothing for
that whole media class. The phase 12 sign-off recorded the opposite — *"stills and image
sequences use the same correct sizing path"*. **The path is the same path; its input was
empty.** Measured on the 4096×2304 still: viewer **1280×675, ratio 1.896 against the file's
1.7778**, pillarboxed inside a window built to have no bars; after, **1280×720 exactly**, and a
1920×1080 PNG sequence likewise. **It survived a sign-off because on 16:9 material the error is
6% of the height and looks right** — only comparing numbers finds it, and nothing printed them
until the inspector read `Current scale: Unknown` from the same empty size. Fixed at the two
reads, not at the source: filling that `QImage` would add a full-resolution copy per frame to
serve two reads of a size.

Regression (physical panel, 5120x1440 @ 239.999Hz): 4K H.264 cadence ×3 **100.0%** with
`handler>budget 0 of 119` and every gap in the ~1x bucket; 4444 ×2 **99.8%** at 0 of 260;
`-SnapRelease` `delta 0` full-res planar, `hitch 0`, `land 0`; both lifecycle legs; **25 of 25
transitions**.

**PHASE 13 IS SIGNED OFF (owner, 2026-08-11) and nothing about the Movie Inspector is open.**
The layout and wording are clear; **the metadata-origin labels are useful and do not read as
clutter**, which was the one design choice with no measurement behind it and the reason the
window was built to be judged rather than asserted. **Tagged, untagged, inferred and observed
information are distinguished honestly** — that is the spec's hardest rule accepted on the
evidence rather than on the implementation. Codec, pixel format, bit depth, dimensions,
viewport size, frame rate, file size and timecode all read correctly across the representative
files, and **the window stays modeless and does not interfere with the floating transport's
auto-hide** — the `holdVisible` decision confirmed by eye as well as by its control.

Read it at its stated width. What was accepted is **the inspector's contents, its wording and
its origin labels**; it is not a sign-off on the menus around it (phase 14 restructures them),
and **the accessibility position is unchanged** — the inspector is a real Qt widget tree and is
therefore reachable by construction, but plan §31.5 item 4 still stands for the *overlay*, which
is not final until a screen reader has driven one.

**ONE FIELD IN THE SIGN-OFF IS NOT A FIELD IN THE WINDOW: there is no Duration row.** The
owner's list named duration; the inspector has never had it. The spec's field list for the
Movie Inspector does not ask for one either — General is filename, source path, resolution,
file size, overall data rate, current viewport size, container, video format, audio format, and
Video details is the fps rational and decimal, bitrate, pixel aspect, display aspect, current
scale, pixel format, bit depth, the four colour tags, codec/profile and track ID. Duration is
on the dev HUD (`dur 5.042s`) and the transport prints frame counts, so it was almost certainly
read there. **Recorded as a discrepancy rather than as a verified field**, because a sign-off
that covers something the window does not show is exactly the kind of widening this project
keeps having to undo. **It is a one-row addition if wanted — `VideoMetadata::durationSeconds`
is already read at open — and it belongs to phase 14 or to an owner decision, not to a phase
that has just been closed.**

**THE PHASE 12 STILL / IMAGE-SEQUENCE BEHAVIOUR IS RE-SIGNED-OFF ON THE CORRECTED BUILD
(owner, 2026-08-11).** Media-shaped windows for stills and image sequences now use the correct
aspect ratio and framing. **This REPLACES the phase 12 sign-off for that media class**, which
was taken on a build where §4 had never applied to it at all: `LoadedImageInfo::image` is left
default-constructed, so `currentDisplayAspect()` read an empty `QSize` and declined. The
original sign-off is not wrong about what was observed — on 16:9 material the error is 6% of
the height — it was taken on material that could not show the fault. The video sign-off from
phase 12 is untouched and did not need replacing.


**SPEC PHASE 14 IS DONE (2026-08-11): the menus are the spec's, the Keyboard Shortcuts window
is generated, and the composited transport is visible to a screen reader.** Audit first, at
`docs/interface-pass-1-phase-14-audit.md`. **NOTHING IN THE PHASE IS SIGNED OFF** — owner
visual and behavioural testing of Loop, 0.5×, Copy Current Frame, the menus, Help and the
Narrator listen are all **pending**.

**THE THREE CONTESTED FEATURES ARE EACH IN THEIR OWN COMMIT so any of them can be taken back
out without touching the menu structure** (owner instruction, 2026-08-11): `78bc67b` audit ·
`bccd21e` menus/shortcuts/Help/window commands · `eeea986` accessibility · **`d9a4840` Loop** ·
**`a218643` 0.5×** · **`5de3552` Copy Current Frame** · `a78dedc` harness · `f603e22` gitignore.
The first cut bundled all three into the menu commit; reverting any one of them would have
taken the menus with it.

**"INDEPENDENTLY REVERTABLE" WAS CHECKED RATHER THAN ASSUMED, AND THE FIRST ATTEMPT FAILED.**
`git revert` on Loop conflicted twice — not because the features interact, but because Loop's
one-line `setEnabled` sat directly beside Copy Current Frame's, and `loopWrap()` directly beside
`copyCurrentFrame()`. **Two independent one-line additions on adjacent lines conflict on
whichever is reverted second**, because git can only see that they touch. The fix is pure code
motion: the pre-existing `speedActions_` loop separates the two one-liners, and
`copyCurrentFrame()` moved past `syncMediaDependentActions()`. Verified as a reordering by
sorting both files and diffing — identical as a multiset of lines, plus six comment lines — and
**all three now revert cleanly and the reverted tree builds, checked one at a time.** The rule
worth keeping: *if two commits must be separately revertable, their edits must not be
adjacent.*

**THE PHASE WAS SPLIT AND THE OWNER ACCEPTED THE SPLIT.** The spec's "menus, help and
accessibility polish" names twenty-five entries and they are four kinds of work: **fifteen are
wiring** over shared `QAction`s that already exist, **four are small net-new behaviour**, **two
are playback-path work**, and **four are renderer work** — Actual Size, Fit to Window, Zoom In,
Zoom Out change the *fit*, which drives the scrub preview size and cache depth (§22.8), and
Actual Size on 4K puts the picture larger than the viewport, needing a pan model Trace has never
had. So **view scaling is phase 15 with its own regression and full regression is 16.**
`Ctrl+0` stays unclaimed until phase 15 takes it with Actual Size.

Owner rulings, each at its stated width: **Check for Updates is OMITTED** (the spec conditions
it on an updater and none exists — absent, not greyed); **Report an Issue is a pre-filled
`mailto:` to `bigsbypuglise@gmail.com`** carrying the build identity, chosen over the private
repo because a link there is a dead end for any tester who is not the owner; **the inspector's
carried Duration row is ADDED**, origin `encoded` because it is the container's claim; and
**Loop, 0.5× and Copy Current Frame were taken INTO phase 14** against the audit's
recommendation to defer them — so this phase contains playback-clock work and a decoder-side
conversion, and priority 1 binds it as hard as any engine phase.

**One correction to the handoff, and it is the ninth premise-expiry.** It said the D3D11
reduction taps were an open decision for upscaling. `updateReduction()` already gates on
`fitted < content` on both axes and carries the reasoning verbatim — step 9 covered it. What
*is* open is the **magnification filter above 1:1**: the sampler is bilinear, and a review tool
at 4:1 probably wants nearest. That is a picture decision on a step-9-signed-off path and it is
phase 15's.

Six things to carry.

- **`audioShouldDrive()` READ `speed <= 1.0001`, WHICH IS TRUE AT 0.5×.** Correct for as long
  as 1× was the slowest rung there was, because "at most 1×" and "exactly 1×" then named the
  same set. At 0.5× the device would run at real time while the picture ran at half, and the
  tick would skip frames chasing the audio clock — **§29.3's fault, reproduced by a comparison
  operator**, and a "never skip a frame" violation outside the sanctioned exception. It is
  `== 1.0` now. **0.5× is silent, and that is the standing rule applied rather than a new
  decision.** Verified with its control on a clip with sound: 0.5× reads `audio … idle`,
  `proc 0ms`, `clk 0.000s`; 1× reads `MASTER`, `proc 2050ms`. The tick's clamp was `max(1.0,…)`
  too, which would have rounded 0.5× silently back up to 1× — the menu ticking 0.5× over a file
  playing normally. `kMinPlaybackSpeed` names it once so the two cannot disagree again.
- **FOUR DUPLICATE MNEMONICS, THREE INTRODUCED HERE AND ONE SHIPPING SINCE PHASE 7.** Two items
  in one menu sharing an Alt key makes it **cycle the highlight instead of activating either**.
  Phase 10 hit this once and fixed it by inspection. `warnOnDuplicateMnemonics()` walks the
  built menu bar at startup and prints to stderr — **a warning, not an assert** — and found
  `SMPTE &Timecode` against `Go to &Timecode` **on its first run**, a phase 7 defect nobody had
  noticed. A new menu item that collides now says so.
- **`syncMediaDependentActions()` WAS NOT CALLED AFTER A SUCCESSFUL OPEN**, so Close Media, Copy
  Current Frame, Loop and every speed rung stayed disabled for the life of the process. **A
  disabled `QAction` does not report being triggered**, so Ctrl+C put nothing on the clipboard
  and the menu item showed no message — which reads exactly like a broken conversion rather
  than a command that never ran. It is now the one place a command is gated on media.
- **COPY CURRENT FRAME IS NOT A CLIPBOARD ONE-LINER, and the spec's "only if safely supported"
  was earned.** Since GATE C a full-res frame on `d3d11` is three YUV planes and
  `VideoFrame::toQImage()` returns **null** for one *by construction* — `qtFormatFor()` refuses
  planar on purpose. So the obvious implementation works under `TRACE_RENDERER=cpu` and puts
  nothing on the clipboard on the shipping build. `VideoDecoderFFmpeg::frameToRgbImage` uses
  **its own swscale context**, never the four-slot LRU playback depends on, and the **frame's**
  colorimetry rather than the decoder's current state. It **refuses a preview-resolution frame**
  rather than copying a soft one. Measured: 1920×1080, correct picture and colour.
- **ALWAYS ON TOP GOES THROUGH `SetWindowPos(HWND_TOPMOST)`, NEVER `setWindowFlag`.** Changing
  a top-level widget's window flags makes Qt **destroy and recreate the native window**, and
  the D3D11 swapchain's surface is a child HWND created once from the viewer's `winId()` — the
  obvious implementation would orphan the surface the picture is presented into, on the default
  renderer, the first time anyone ticked the box. Verified by reading `WS_EX_TOPMOST` **off the
  window** rather than the menu tick (`False → True → False`) and capturing the picture after.
- **LOOP IS ANSWERED IN ONE PLACE.** Three sites in the tick reach "playback reached the end" —
  the shuttle off the tail, a `Playback` decode finding nothing left (**only long-GOP media
  reaches this one**), and the target clamping to `maxFrame`. Before Loop they only had to agree
  on a flag; **a wrap that happens at two sites of three is a file that loops except when it
  does not, depending on the codec.** It applies to a shuttle run too. **`loop N wraps N` is on
  the HUD because a wrap RE-ESTABLISHES the playback timeline and zeroes every cadence counter
  beside it** — a figure from a looping run is one lap's.

**THE ACCESSIBILITY PROXY TREE IS BUILT AND WAS DRIVEN, AND DRIVING IT FOUND FOUR THINGS.**
`scripts/measure/uiatree.ps1` walks the same UI Automation tree Narrator consumes.

**It broke the Space bar.** Plan §19.7 asks for proxies "in a real tab chain"; the first cut
gave them `Qt::TabFocus`, Qt assigns initial focus to the first focusable widget in the window,
every other transport widget is `NoFocus` by an old rule and so is the viewer — so the **Rewind
proxy took focus at show time and the first Space press on a fresh launch read `speed -2.00x |
Reverse Play`**. The rule it broke names the failure exactly: *"Transport widgets must not take
keyboard focus … if a new widget steals arrows/space, this is why."* Reading that as a rule
about `QPushButton`s rather than about anything occupying the transport was the mistake.
**`Qt::NoFocus` now — a narrower claim than §19.7 made, not a workaround for it**: screen
readers navigate the tree rather than the tab chain, and every command already has a shortcut
that works from anywhere. Switching it back is not a one-line change; something has to hold
focus by default first.

**Every proxy had an EMPTY BOUNDING RECTANGLE.** `OverlayModel::layout()` runs inside
`buildFrame()`, i.e. **during the paint**, so proxies synced from `resizeEvent` read the rects
from before the layout that resize caused — and at startup before there had been any layout at
all. All five controls were correctly named, in the right reading order, and **a screen reader
could not have said where any of them were.** Phase 10's trap in a third costume. Fixed with
`OverlayModel::layoutRevision()`, the same promise `atlasRevision()` makes, checked once per
paint. The rects now match phase 6's settled geometry to the pixel: **44×44 play, 34×34
utility.** **Every control also announced as "group"** (a plain `QWidget` maps to
`QAccessible::Client`); they are `Button`/`ButtonMenu`/`Slider` now. **And the Button role made
Qt advertise Invoke, which did NOTHING** — `QAccessibleWidget` presses a `QAbstractButton` and
these are plain widgets, so it was **a control that appears actionable and is not**, the
`showInfo` failure phase 2 deleted, hidden where only a screen reader would find it. Invoking
Fast-forward through UIA now — **no focus, no click** — reads `speed 2.00x | FF`.

**THE NEGATIVE CONTROL INVERTED A CLAIM THIS PROJECT HAS CITED SINCE PHASE 6.** The same walk
under `TRACE_TRANSPORT_BAR=1` — the escape hatch, described in five documents as the
accessibility mitigation because it *"restores real widgets"* — found five controls reported as
**`Group ""`**. The widgets were real and had **no accessible names**, because they are
icon-only buttons whose `text()` is empty, and `TransportButton` is a plain `QWidget` so it had
no role either. **The mitigation was announcing almost nothing**, and before it was fixed the
composited overlay's proxy tree was already strictly better than the bar it was written to make
up for. Both are fixed, so the claim is now true rather than nearly true.

Read the accessibility result at its stated width: **UIA is the interface Narrator consumes, so
an element absent from that tree is certainly not announced — but an element present in it can
still read badly aloud**, which is why walking the tree could never close §31.5 item 4 and this
record left it open. **It IS closed now — the owner listened on 2026-08-11 and it passed; see
the phase 14 sign-off block above.** What remains true here is the *reason* the walk was not
enough, not the open item.

**Regression — flat (physical panel, 5120x1440 @ 239.999Hz):** 4K H.264 cadence ×3 **100.0%**
with 120 frames, `handler>budget 0 of 119` and all 119 gaps in the ~1× bucket; 4444 ×2 **99.8%**
at 0 of 260; `-SnapRelease` `target 120 shown 120 delta 0` full-res planar with **`hitch 0`**;
both lifecycle legs; **25 of 25 transitions**. Case for case with phases 12 and 13.

**THE FIRST CADENCE RUN WAS WRONG AND EVERY FIGURE ON IT LOOKED PERFECT.** It read `frames 30 |
elapsed 1.25s` against a 119-frame baseline with `presented 24.00/24.00 (100.0% real time)` on
the same line — **Loop was on, left on by the loop harness's own first run**, and every wrap had
reset the counters. **`cadence.ps1` now runs against a scratch `TRACE_SETTINGS_FILE`** so it
cannot inherit the machine, the way `recentfiles.ps1` already did; and the HUD's `loop` field is
what to read first if a cadence figure is ever questioned. **A persisted preference is an input
to a measurement.**

**Two harness lessons that generalise past this phase.** `SetForegroundWindow` **fails silently
from a background process** — a child PowerShell's own terminal takes focus, every `SendKeys`
went there, and the run reported the feature missing; use `AttachThreadInput` and **read
`GetForegroundWindow()` back** rather than assuming. And **P/Invoke's default is
`CharSet.Ansi`**, so a managed string handed to any `...W` function is marshalled as ANSI and
read back as UTF-16 — window titles came out **one character long**, and "Keyboard Shortcuts
window NOT FOUND" was printed three times against a build where it was open on screen.

**PHASE 14 IS FULLY SIGNED OFF (owner, 2026-08-11), in two parts taken on different days.**

**Part 1 — Loop, 0.5× and Copy Current Frame are ACCEPTED.** All three behave as intended. That
closes the revert question with them — they are shipped features, not extractable candidates —
so **`kMinPlaybackSpeed`, `audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and
`frameToRgbImage`'s own swscale context are settled behaviour**, and changing any of them
re-opens an owner decision rather than being a tidy-up. Loop's persistence across a file change
and a restart was accepted with the feature: it is a review preference, not a property of the
media.

**Part 2 — THE MENUS AND HELP WORDING ARE ACCEPTED, AND THE NARRATOR LISTEN IS DONE. THESE WERE
THE LAST TWO OWNER ITEMS IN THE ENTIRE INTERFACE PASS.** The menus were read **as they now
stand**, which is wider than the review first asked for — phase 15 added four rows to View and
two to Window afterwards — and Trace Help's four paragraphs, the only prose in the product, were
read rather than skimmed. That was the last wording question in the pass.

**PLAN §31.5 ITEM 4 IS CLOSED.** Narrator discovers the floating transport; **Play/Pause,
Rewind, Fast-forward, Timeline and Share are announced clearly and activate with
Narrator+Enter**; ordinary Space play/pause is unaffected. The composited transport is
**genuinely usable by a screen reader, not merely present in the UIA tree** — the distinction
every prior record was careful to keep open, now answered by **listening rather than by
walking**. `uiatree.ps1` stays the regression check; what it could never do is close this item.

**IT IS REACHED WITH THE NARRATOR CURSOR, NOT THE TAB CHAIN, AND THAT IS DELIBERATE — a future
session will otherwise read it as a defect.** §19.7 asked for proxies in a real tab chain; phase
14 built exactly that and it **broke the Space bar**, because Qt gives initial focus to the first
focusable widget and the Rewind proxy took it. They are `Qt::NoFocus`, which is the narrower and
correct claim: screen readers navigate the accessibility tree, every command already has a
shortcut that works from anywhere, and staying out of the tab chain protects playback keyboard
focus at no cost to a screen-reader user — which is what the listen confirms. **Do not "fix"
this by restoring the tab chain**; that is the regression `eeea986` folded in, and reversing it
needs something else to hold focus by default first.

**EVERY OWNER ITEM IN THE INTERFACE PASS IS NOW ANSWERED.**

**SPEC PHASE 15 IS DONE AND MEASURED (2026-08-11, `aae42bf` render · `55fd965` app · `163c439`
harness): the picture has a scale and an origin, and above 1:1 it shows pixels.** Actual Size,
Fit to Window, Zoom In, Zoom Out and the pan they imply. **SIGNED OFF by the owner on
2026-08-11 after one correction — see the sign-off block below for what was accepted and at
what width.**

**Two owner decisions were taken BEFORE implementation, on instruction**, because both are
picture-and-geometry choices that must not be settled quietly inside a phase: **above 1:1 the
picture is NEAREST, not bilinear** (a review tool at 4:1 is inspecting samples, and the CPU path
already had the shape of that rule at exactly 1:1); and **Actual Size does NOT resize the window,
it pans inside the viewport** — §4's media-shaped window, its 1280x720-equivalent area cap and
its 80% work-area rule were signed off two days earlier and a window grown to fit 4K would break
all three.

Six things to carry.

- **THE SCALE'S REFERENCE IS THE FULL-RESOLUTION SOURCE, NOT THE DELIVERED FRAME, and that is
  the whole of `ViewScale::referenceDisplayed`.** A backend knows only the frame it holds, and
  during a drag that frame is a **preview converted to the viewport's size** — 1066x600 standing
  in for 3840x2160. Scaling it would draw Actual Size at 1066x600 mid-drag and 3840x2160 on
  release: **the picture would jump 3.6× at the moment the user stopped.** Measured with the
  button still held: `dst RGB32/BGRA 1066x600` while `display 3840x2160 zoom 1.00:1`, landing
  `target 56 shown 56 delta 0`. **The consequence, stated rather than hidden: at a zoom the
  preview is under-resolved**, because it is sized to the viewport while a zoomed viewport shows
  a *crop*. Soft during motion, exact on release — the standing rule, not a new exception. If it
  is ever worth fixing, **crop the conversion, do not enlarge it**; enlarging is the §22.8 risk.
- **`viewDeviceRect()` is the one expression, and the FIT BRANCH IS THE OLD ARITHMETIC reached
  by one test.** A "unified" version that fitted by computing a fit scale and running it through
  the scale path would be the same answer by a different route, and **every recorded fit figure
  in this project would then be comparing against arithmetic it was not taken on.**
- **`maxViewScale()` IS A HARD BACKEND LIMIT, NOT A TASTE JUDGEMENT.** D3D11 letterboxes by
  **viewport**, and a `D3D11_VIEWPORT` past `D3D11_VIEWPORT_BOUNDS_MAX` is **rejected outright —
  it draws nothing at all, silently.** 4K reaches 8:1; a 9K plate stops at 2:1. The CPU backend
  honours the same cap, because an escape hatch that zooms further than the shipping renderer is
  exactly what nobody would think to check.
- **The ladder DOUBLES and is anchored on 1:1, for a reason that outlives taste**: at an integer
  power of two, point sampling replicates each sample into an **exact square block**, while
  nearest at 3.2:1 draws some samples three device pixels wide and others four. Zoom In from fit
  takes the first rung strictly above the fit's arbitrary ratio, so one press always visibly
  zooms. Measured 4K H.264, `win 1201x1083` on every rung: fit 0.31 → **0.50 → 1.00 → 2.00 →
  4.00 → 8.00 → 8.00 (pinned)**, and back down through 0.50 → 0.25 → 0.13. **The reduction taps
  track the fit** — x1 at 0.5, x2 at 0.25, x4 at 0.125 — which is the check that step 9's gate
  still holds under a scale. Same ladder and same pinning on 4:5 ProRes at `win 540x1082`.
- **PANNING GOES THROUGH `OverlayHooks`, and that is not where it looks like it belongs.** Under
  the D3D11 backend the video is a **child HWND that takes every mouse message** and Qt's widget
  never sees one, so a pan written in `ViewerWidget::mousePressEvent` would work on
  `TRACE_RENDERER=cpu` and **do nothing on the shipping build** — which is exactly why
  `toggleFullscreen` is already in that struct. `OverlayModel` routes a press landing on no
  control to `canPan`/`panBy`, **ungated on `enabled_`** so the gesture survives
  `TRACE_TRANSPORT_BAR=1`. Measured with its negative control: **drag at fit `0%, max delta 0`;
  the identical drag at Actual Size `31.7%, max delta 58`.**
- **`repaint()`, NOT `update()` — phase 10's trap in the second thing that has it.** The first
  build magnified the picture visibly while `display` still read the fit's
  `1201x676 filtered x2`, which is precisely what a build whose zoom never reached the renderer
  would print. **Sixth stale-instrument finding**, and the second from this exact mechanism.

**The magnification filter point-samples CHROMA too, and that is the honest reading rather than
an oversight** — on 4:2:0 a chroma sample really does cover four luma samples, so at 4:1 it
really is an 8×8 block of one colour. Filtering chroma while point-sampling luma would draw
colour detail the file does not carry, at the one magnification where someone is looking for
exactly that distinction. **It is the most likely thing in this phase to read as a defect while
being correct**, and `TRACE_MAG_FILTER=linear` is the side-by-side on both backends — and also
how the whole decision is reverted without touching the rest of the phase. The CPU path filters
**only when reducing** now, one predicate that *subsumes* the old exactly-1:1 case rather than
sitting beside it, so that long-standing behaviour is unchanged.

**Cost:** D3D11 at 4:1 reads `draw 0.01ms`; the CPU backend reads `draw 1.18ms` of a 41.67ms
budget, because QPainter clips the blit and the cost tracks the **visible** area rather than a
15360x8640 destination. **Both backends agree on every rung.** Nothing is persisted, so **no
later measurement can inherit a zoom** the way phase 14's first cadence run inherited Loop.

**Regression — flat, case for case with phases 12–14** (physical panel, 5120x1440 @ 239.999Hz):
4K H.264 cadence ×3 **100.0%** with 120 frames, `handler>budget 0 of 119` and all 119 gaps in
the ~1× bucket; 4444 ×2 **99.8%** at 0 of 260; `-SnapRelease` `target 120 shown 120 delta 0`
full-res planar with `hitch 0` and `land 0`; both lifecycle legs; **25 of 25 transitions**.

**`transitions.ps1` WAS SEEING THROUGH THE WINDOW BORDER, and it accused a correct build for all
25 cases.** `GetWindowRect` includes Windows 11's invisible resize border, so the first captured
columns are **whatever is behind Trace**; with a bright window behind, the control scan found
`clusters=4 -> 3,46,107,165` and reported "groove or controls not located" 25 times. It is
neither clip- nor build-dependent — **it depends on what is on the desktop**, which is why the
same matrix passed minutes earlier on another file. The scan starts 16px in now. **And the clip
is part of the measurement, again**: on the 121-frame 422 HQ clip the matrix FAILs `F -> ffBtn`
with `moved 0%`, exactly as that harness's **own header** warns, and **a control binary built
from `9db4780` failed the same case with the same `0%` and `5.6%` to the digit** — which is what
told it apart from a regression. `M&M_TopGun_1080.mp4` is the clip the header names.

**PHASE 15 IS SIGNED OFF (owner, 2026-08-11), with one correction applied first.** Four things
were accepted and each is now settled behaviour rather than a default: **nearest magnification
above 1:1 stays** — so the **point-sampled chroma is accepted behaviour, not a carried defect**,
and `TRACE_MAG_FILTER=linear` is a comparison knob rather than a pending revert path; **the
pan's behaviour is correct**; **Fit to Window takes no default shortcut**; and **the
under-resolved preview during zoomed scrubbing is accepted**, on the stated condition that the
exact full-resolution image returns immediately on release **with no position jump** — which is
precisely the property `ViewScale::referenceDisplayed` exists to guarantee and which was
measured (`dst RGB32/BGRA 1066x600` mid-drag while `display 3840x2160 zoom 1.00:1`, landing
`target 56 shown 56 delta 0`). **That is the sixth instance of the motion-over-fidelity rule,
not a new exception.**

**THE CORRECTION: Fit to Window stays ENABLED while active and shows its checked state.** The
first cut greyed it while already fitting, on the reasoning that a command which visibly does
nothing is the `showInfo` failure. **That reasoning does not transfer to a CHECKABLE item** —
the tick is what states the current state, and greying the row makes "this is what the picture
is doing" read as "this is unavailable". Actual Size was already enabled while checked, so the
two rows behave alike now. Verified through the **menu**, not the key: Alt+V then `w` takes
`display 3840x2160 zoom 1.00:1` back to `display 1201x676 filtered x2` with the `zoom` field
gone, and **selecting it again from the checked state is a harmless no-op**. Ladder and pan
re-measured after it and identical **to the digit**; regression flat, 25 of 25 transitions.

**THE PAN CURSOR LEAVES THIS PHASE AS POLISH, WITH ITS CONDITION STATED** (owner): a grab /
closed-hand cursor would improve discoverability, is **not required**, and is wanted only if it
can be made trivial **and identical on both backends**. It cannot today — the D3D11 surface owns
its own window-class cursor and answers `WM_SETCURSOR` itself while the CPU path inherits the
widget's. That is the same two-mechanism split `setCursorHidden()` already carries, and phase 6
measured `GetCursorInfo` seeing only one of the two. So it is not a one-liner and must not be
described as one.

**Read the sign-off at its stated width**: what was accepted is the **scaling behaviour, the
magnification filter, the pan and the zoomed-scrub trade**. It was **not** a sign-off on the
menus and Help wording — phase 14 left those open and this phase added four rows to them — nor
on the Narrator listen. **Both of those were then accepted separately on 2026-08-11** (phase 14
sign-off part 2 above), which is what left the pass with no open owner item; they are not
covered by *this* sign-off and were never meant to be.

**SPEC PHASE 16 IS DONE AND THE INTERFACE PASS IS CLOSED (2026-08-11).** The full regression,
the spec-and-rulings audit, and both owner items. Record in `docs/interface-pass-1-progress.md`
under "Phase 16". Display: **physical panel, 5120x1440 @ 239.999Hz** throughout.

**THE HANDOFF'S ONE CONFIRMED MISS DOES NOT EXIST, AND FINDING THAT OUT IS THE PHASE'S MAIN
RESULT.** It stated that the Movie Inspector still had no Duration row, quoting the phase 14
owner ruling from three documents and a verification at HEAD. **The row was built at phase 14 in
`bccd21e` and is live at `MainWindow.cpp:2276`** — confirmed on screen, not by reading:
**`Duration: 0:05.042 (121 frames)`**, origin **`encoded`**, which is the ruling's stated origin
followed exactly. **The grep could not have found it**: it was aimed at `MovieInspector.cpp`,
which by design holds no fields at all — no `QFile`, no `QFileInfo`, no decoder, no viewer, it
takes a value type — and every row is built in `MainWindow::buildInspectorSnapshot()`.

**Seventh stale instrument to accuse a correct build**, after phase 8's menu-icon luminance,
phase 9's un-refreshed HUD, phase 10's HUD after a transform, phase 12's HUD on resize, phase
14's ANSI-marshalled window titles and phase 15's HUD after a view scale — and **the first that
is a grep aimed at the wrong file**. The rule: *a negative grep is only evidence if the thing
being grepped for would have to be in that file.* A near-miss the same session went the other
way — the **Audio details** section looked absent from a capture and is simply below the fold of
a 739px window, with all five spec fields built at `MainWindow.cpp:2501–2530`.

**THE AUDIT FOUND NOTHING MISSING.** All five menus match the spec exactly (Check for Updates
correctly **absent**, not greyed); every shortcut the Keyboard section names is registered,
including **M for Mute**, the spec's "if unclaimed" case; zero-based numbering holds at the right
endpoint on all three media classes (`resetForNewMedia(frameCount - 1)`, a 168-file sequence
reading `Frame: 136/167`, a still reading `Frame: 0/0`); §4's `Lock Window to Media Aspect Ratio`
is present, checked by default and persisted; and **the shuttle mute policy is implemented rather
than incidental** — `AudioOutput::start()` re-applies the stored `impl_->muted` to the new sink
before starting it. One item is carried and is a **design-package** detail rather than a spec
one: the rate chip is top-left, the package's §6 puts it centred above the transport.

**REGRESSION — FLAT ACROSS THE WHOLE ASSET SET, and four of these files had never been measured
since the interface work began.** Cadence with `TRACE_NO_AUDIO=1` and a scratch
`TRACE_SETTINGS_FILE`: **4K H.264 x3 100.0/100.0/100.0%** (120 frames, `0 of 119`, all 119 gaps
in the ~1x bucket) · **4444 x2 99.8%** (`0 of 260`) · **1080p x2 100.0%** (`0 of 239`) · **4K
60fps x2 100.0%** (`0 of 161`, against a **16.67ms** budget, the tightest in the set) · **422 HQ
x2 99.9%** (`0 of 167`) · **1x1 and 4x5 ProRes x2 each 100.0%**, every gap ~1x, `hitch 0`. All
16:9 files at `display 1226x690` / `win 1226x1083`; 1x1 `690x690`/`690x1083`; 4x5
`552x690`/`552x1083`. `scrub -SnapRelease` `target 120 shown 120 delta 0` full-res planar with
**`hitch 0`** and `land 0`; both lifecycle legs (83.6% and the 0% control); **25 of 25
transitions**; the still and the image sequence both §4-shaped and zero-based; `uiatree.ps1`
still reporting five named, correctly typed controls on the drawn rects. **Case for case with
phases 12–15.**

**`handler>budget` IS NOT READABLE ON THE 1x1 AND THE 4x5** — §4 makes those windows narrow and
the dev HUD clips, which is the **phase 12 diagnostic limitation reproducing**, not a defect.
What bounds it is readable: 100.0% of real time with every one of 322/318 gaps in the ~1x bucket
and none in any other, handler ~2ms against a 41.71ms budget, `hitch 0`. Recorded as an
inference, because it is one.

**ONE HARNESS FAULT, AND IT WAS THE INVOCATION.** `transitions.ps1 -All` reported
`FAIL - groove or controls not located` on **all 25 cases** — the exact signature of phase 15's
window-border fault. It was neither that nor a regression: the run **omitted
`-Env TRACE_TRANSPORT_BAR=1`**, which the matrix needs because it finds every control by scanning
the docked bar, and phase 6 took that bar out of the layout by default. The script's own param
block says so. Re-run with it: **25 of 25 PASS.** *All 25 failing the same way is a statement
about the harness's inputs, not about the build* — check the invocation against the script header
before building a control.

**BOTH GPU PREREQUISITES ARE BUILT AND MEASURED (2026-08-10, plan §31), and the spec's own
phase 1 audit is `docs/interface-pass-1-audit.md`.** Playback and scrub are unchanged across
both: cadence 100.0/99.9% of real time with `handler>budget 0 of 119`, scrub reversals
`hitch 1`, `delta 0`, `-SnapRelease` landing exactly, both lifecycle through-drag gestures
passing. Read plan §31 before touching either.

- **The overlay is a real path on BOTH backends** (`5e1f834`). `OverlayModel` owns layout,
  art, fade, hit-testing and the hooks and emits **quads**; `D3D11OverlayDrawer` and
  `CpuImageRenderer` each just draw them. The two agree because the compositing arithmetic is
  the same on both, and **pixel-snapping the layout is what made them agree**: before it the
  play glyph differed on 8.1% of its pixels at max delta 29, purely from two resamplers
  reconstructing the same art at a fractional offset; after it, 0.0% at max delta 1. **Still
  OFF by default** — the mechanism is real, the artwork is still placeholder until phase 2,
  and enabling it now puts two transports on screen. `TRACE_OVERLAY` (or the retained
  `TRACE_OVERLAY_COMPOSITED`); the HUD reads `+overlay`.

  **The first cost control was not a control**: "the same drag with the overlay off" has no
  overlay track to drag, so it measured a drag against no drag (`paints 0/1`). The real
  control is the transport-groove drag with the identical reversal sequence
  (`scripts/measure/overlay_drag.ps1`). Result: `hitch 1` either way, landing exact either
  way, and the only cleanly attributable cost is **+0.05ms per paint on the CPU backend**
  (0.23 → 0.28ms against a 41.67ms budget). The `ui gap max` gap in the overlay's favour
  (17 vs 84ms, repeatable) is **unattributed — do not quote it as a win**.

- **`VideoRenderer` has a view-transform contract** (`4b7174f`). D3D11 applies it in the
  **vertex shader's texture coordinate**, which is why neither pixel shader changed and every
  subsampling, bit depth and the box average inherit it without a variant. Two things had to
  follow it and both fail silently: the **fit** (a quarter turn re-letterboxes — measured
  `display 640x360` → `202x360`, identical on both backends) and the **reduction taps**
  (`filtered x3` → `x4` at rot90, recomputed from the post-transform fit with the footprint
  axes exchanged). The CPU path names `scale` before `rotate` deliberately — QPainter
  post-multiplies, so the other order turns `rot90 + flipH` into `rot90 + flipV` and the two
  backends would differ by a mirror while every number agreed. **Spec phase 10 wired the Edit
  menu's five actions to this and both predictions above were confirmed to the digit**;
  `TRACE_VIEW_TRANSFORM` left with it. The HUD reads `view rot90 flipH`.

Owner context: Anj is a VFX/motion-design lead, not a programmer. Explain things plainly; he tests builds on a Windows RTX 4090 box; development happens on macOS. Don't ask him to debug code — give exact copy-paste terminal commands when he needs to run anything.

## Build and test

No test suite yet. **GitHub Actions is the source of truth for release builds**, but the Windows box has a full local toolchain — use it to catch compile errors before pushing.

### Local build on the Windows box (Aug 2026 — verified working)

Qt 6.10.2 (msvc2022_64, includes Multimedia), vcpkg FFmpeg 8.x (avcodec-62), and VS2022 Community are all installed. None are on `PATH`, so call them by full path:

```
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\6.10.2\msvc2022_64" -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release --target Trace --parallel
& 'C:\Qt\6.10.2\msvc2022_64\bin\windeployqt.exe' --release --no-translations 'build\app\Release\Trace.exe'
```

FFmpeg DLLs are already in `build\app\Release`; `windeployqt` supplies the Qt runtime, `platforms\qwindows.dll` and the multimedia plugins. Then run `build\app\Release\Trace.exe`. **Configure prints `Trace: audio output enabled` or `DISABLED` — check that line**, and since GATE B also `Trace: D3D11 renderer enabled` or `DISABLED (needs Windows + MSVC + fxc)`. The D3D11 backend needs `fxc` from the Windows SDK to compile its shaders at build time; if CMake cannot find it the backend is left out and the app builds exactly as before, so a `DISABLED` line is a missing SDK rather than a broken tree. Note local Qt is 6.10.2 while CI pins 6.7.2, so a local green is not proof CI is green; it does catch every compile error.

- Repo: `https://github.com/bigsbypuglise/trace-alpha` (GitHub account: bigsbypuglise; private)
- Every push to any branch builds Windows (VS2022, Qt 6.7.2 via install-qt-action, FFmpeg via vcpkg) and uploads artifact `trace-windows-x64` (workflow: `.github/workflows/windows-release.yml`)
- Tags matching `v*` also publish a GitHub prerelease with a `trace-windows-x64.zip` asset
- **The package name is `DIST_NAME` at the top of the workflow and is spelled ONCE.** It carries no release stage on purpose. Note the **repository** is still `bigsbypuglise/trace-alpha` and that is the remote URL — a search-and-replace over `trace-alpha` breaks the remote, and `docs/release-notes-alpha.md` keeps its filename so links to it do not break.
- **The artifact is uploaded as a folder, never as a .zip** (Aug 2026): `upload-artifact` always zips its input, so uploading a zip produced a zip-inside-a-zip and Anj's download had no runnable app in it. Release assets are *not* re-zipped, so tags still build a real ZIP.
- **Green must mean launchable** (Aug 2026): the workflow checks native tool exit codes (`windeployqt` failures used to pass silently), asserts FFmpeg was found at configure time, and verifies `Trace.exe` + Qt DLLs + `platforms/qwindows.dll` + av* DLLs exist before publishing. If a build goes green, the download starts.
- **CI checks the interface assets against the `.qrc` contract, before the build** (2026-08-15, `3e0c936`): `scripts/verify_trace_assets.py --strict --no-pillow`, second step in the workflow because it needs no toolchain. It catches the class `rcc` cannot — a 25px export named `-24`. Its set is **derived** from `app/resources.qrc` and `app/trace.rc`; see the asset-tree entry above for why that is what made it CI-safe.
- **CI asserts the renderer initializes** (Aug 2026, `b5ad4d2`): `Trace.exe --renderer-selftest=d3d11` builds the viewer, lets it adopt whatever `TRACE_RENDERER` selects, prints `renderer=`/`fellback=`/`planar=` and exits. It runs the real path — `ViewerWidget`'s constructor applies the native-surface contract and calls `initialize()`, which creates the device, the child surface window, the flip-model swapchain, every shader and the render target. **No `show()`**: `initialize()` reaches the HWND through `winId()`, so the check does not need an interactive desktop. The match is a **prefix**, so a runner that falls back to the software rasteriser and renames itself `d3d11 (warp)` still passes. (In the event the first run reported plain `d3d11` — the GitHub runner's device took the hardware path.) **Exit 3 is the selected backend failing to initialize, exit 4 is that backend never having been built** (no `fxc`); the two are separate codes because they are separate faults, and that is also why the expected name is an argument to the exe rather than a grep in the YAML. `planar=1` is asserted too — a failed YUV shader is deliberately non-fatal at runtime (GATE C), which makes it exactly the silent degradation this step exists to catch. **It was printed for one run before being asserted**, because whether the runner's device supplies `ps_4_0` had never been observed and guessing would have turned the first build red on a guess.
- vcpkg/FFmpeg and Qt are cached; the ~20+ min build only recurs on cache miss (7-day idle expiry). Bump `VCPKG_CACHE_VERSION` in the workflow to force a clean FFmpeg rebuild.
- **Whether Claude can push depends on which machine the session is on — check, don't assume.** On the **Windows box** (repo at `C:\Users\andre\Documents\Claude_Cowork\Trace_Windows`) github.com **is reachable and Claude can push directly**; verified Aug 2026 by a read-only `git ls-remote` followed by a real push. On the **macOS sandbox** the proxy blocks github.com, so commits are made locally and Anj pushes from `~/Claude/Trace`.

  **`~/Claude/Trace` is macOS-only and does not exist on the Windows box.** Handing that command to Anj there fails with "no such file or directory" — which happened silently across several sessions, until **23 commits had accumulated unpushed** and no CI run appeared. The instruction was copied out of this file without checking it applied to the machine in use. Before telling anyone to push, run `git remote -v` and `git rev-list --count @{u}..HEAD`, then either push directly or give a path that exists where the session is actually running.
- Manual validation checklist: `docs/windows-validation-checklist.md`
- Local build if a toolchain exists: `cmake -S . -B build && cmake --build build --config Release --target Trace`

FFmpeg and OpenImageIO are optional at compile time (`TRACE_WITH_FFMPEG`, `TRACE_WITH_OIIO` defines). Code touching them must stay inside those guards and compile without them.

## Architecture

Qt6 Widgets app, C++20, CMake. Single executable target `Trace` defined in `app/CMakeLists.txt` (sources live in `src/`).

The core abstraction is `trace::core::FrameSource` (`src/core/FrameSource.h`): a pull-based interface — `frameAt(frameIndex, outImage, error)`. Two implementations:

- `VideoFrameSource` → thin wrapper over `VideoDecoderFFmpeg` (mp4/mov)
- `ImageSequenceFrameSource` → `StillImageLoader` (stills and numbered sequences via `SequenceParser`; OIIO when available, QImage otherwise)

`MainWindow` (`src/app/MainWindow.cpp`) owns everything and drives playback **synchronously on the UI thread**: a `QTimer` at ~1/fps ticks → `PlaybackController` (pure state machine: mode/speed/current frame, J-K-L jog, stepping) computes the target frame → `loadCurrentFrame()` pulls from the FrameSource → `ViewerWidget` paints. `TransportOverlay` is the HUD; `refreshHud()` builds a dev diagnostics line from `VideoPerfStats`. There is deliberately **no decode thread** — an async prefetch pipeline was tried and reverted (commits a171e3a/1d280eb, reverted 9cd2a0c/a2f7999) because it broke frame ordering.

`VideoDecoderFFmpeg` (`src/core/VideoDecoderFFmpeg.cpp`) is where nearly all playback complexity lives:

- **Linear forward decode** is the invariant. Frames map PTS→index via `frameFromPts` with a monotonic bump to prevent frame-order bounce (commit 7a3fa95). Playback decodes exactly one frame per request — steady per-tick cost.
- **Request modes** (`Playback`/`Scrub`/`Step`) change behavior: seeks happen on scrub, backward moves, or jumps >1; sws conversion quality is mode-aware (fast flags for Playback/Scrub, `SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND` for Step, since a paused frame is being inspected). Env overrides for A/B: `TRACE_PERF_FAST_CONVERT=1`, `TRACE_PERF_ACCURATE_CONVERT=1`.
- **Reverse playback/stepping and random-access scrub** work from `reverseCache`, filled with the presented frame plus frames decoded en route to a target; a cache miss triggers seek-to-keyframe + decode forward. Capacity is footprint-derived (6 at 4K, 24 at 1080p) and it is consulted for any random-access request in either direction, not just backward ones.
- swscale is slice-threaded (threads=auto) when FFmpeg ≥ 5.1 at build time.
- **Codec threading is mode-critical** (July 2026): intra-only codecs (ProRes/DNxHD/MJPEG) use `FF_THREAD_SLICE` only. Frame threading pipelines across frames, so every seek+flush (scrub, reverse step) stalled ~thread-count packets (~100ms) before emitting one frame. Long-GOP codecs keep FRAME|SLICE for playback throughput. **This was long assumed to make H.264 scrub/step slow; measurement in Aug 2026 disproved it** — seek plus flush costs only 1–3ms on H.264, and scrub latency is the GOP decode walk instead. Frame threading does have one real consequence: buffered frames must be drained at EOF (see below), or the tail of every long-GOP file is silently lost.

Scrubbing is throttled in `MainWindow` (12 ms single-shot `scrubTimer_` coalesces slider moves; release forces exact frame).

## Decisions already made — don't relitigate casually

- **No async decode thread** until there's a design that provably preserves frame order; the March 2026 attempt was reverted. If revisiting, sequence-number every request and drop stale results.
- **A decoded frame is a `VideoFrame`, not a `QImage`** (Aug 2026, `03d840e`, `src/core/VideoFrame.h`): a refcounted `FrameBuffer` plus `frameIndex`, `ColorInfo` and the `previewRes` tag. Copying one is a refcount bump, which is what makes discarding a superseded async result cost a single decrement. `QImage` is now a zero-copy **read-only view** built over the buffer. Three things follow, and each replaced a rule that had to be remembered with one that cannot be broken:

  **The detach hazard is gone by construction.** `QImage::bits()` is non-const and deep-copied ~38MB at 4K whenever the buffer was still referenced by the viewer or the cache; the pool dodged it by only handing back entries reporting `isDetached()`. swscale writes to `buffer->data()` now, which cannot detach, and the pool's free test is `use_count() == 1`. The `detach` HUD counters are **kept and read 0.00 by construction** — so a regression back to the old behaviour would still be visible.

  **`previewRes` is set by the conversion, from the size it actually converted at.** It used to be predicted at the call site by a second expression that had to agree with the resolution branch inside the converter — and the two read *different widths* (container metadata vs the decoded frame). A frame can no longer be stored at one resolution and labelled another. The old prediction survives only to size the seek-walk fill window, which must be decided before any frame exists.

  **A failed conversion reports failure.** `convertCurrentFrame` returns bool and clears the output on entry. The output is frequently the same object across requests, so the old behaviour left the *previous* frame in it and the caller then stamped it with the new index — one frame on screen under another's name, which is exactly what `e76eabb` exists to prevent.

  Do **not** put `AVPixelFormat` (or any FFmpeg type) in this header: it is reached from `FrameSource.h` and therefore from the image-sequence path, which must compile with `TRACE_WITH_FFMPEG` undefined. `PixelLayout` is Trace's own enum.
- **Superseded requests are dropped in exactly one place** (Aug 2026, `75a3412`): `MainWindow::requestGeneration_` is monotonic and bumped by `supersedeInFlightRequests()` on **every change of target** — not only when storage is busy, because "the target moved" is the condition the async worker acts on and it must mean the same thing whether or not a read is outstanding. `loadCurrentFrame` captures the generation, and discards the result if it changed while the request was in flight. This is `ioCancelCount_` generalised: it was already this counter in all but name, but it only counted remote-I/O cancellations. HUD `gen N drop M` splits the two questions — `gen` counts target changes and climbs through any drag, `drop` counts results actually thrown away. **`drop` is the one to watch**: 0 on local media, non-zero on a slow remote source, and after step 5 a fast drag on heavy media that leaves it at 0 means the worker is not being superseded and something is wrong.
- **Random-access scrub decode runs on a worker, and the decoder is LEASED to it** (Aug 2026, `f77d472`, `src/core/ScrubDecodeWorker.*`, plan §14). There is one `VideoDecoderFFmpeg` and exactly one owner of it at any instant: the UI thread by default, the worker for the duration of a drag. Ownership comes back through **one** function, `reclaimDecoder()`, which is called from `loadCurrentFrame` and `prepareVideoRequest` as well as the explicit transitions — so "the UI thread never touches a leased decoder" is a property of one choke point rather than a convention observed at a dozen call sites. **Playback is untouched and still decodes synchronously**; the worker reads `posted 0` through a playback run, which is the check that it has not crept in.

  While the lease is out the HUD reads a telemetry snapshot the worker publishes with each result. `metadata()` is the one thing read live, because only `open()` writes it and `open()` cannot run while a lease is out.

  **The drag is a pipeline**: one frame requested at a time, chained on delivery, still one frame at a time toward the pointer, still never a jump. `kScrubEase` and `kScrubWalkBudgetMs` are gone from this path rather than ported — both only decided when the *synchronous* loop should yield, and neither ever changed which frames were shown, because the loop always stepped by one. `TRACE_ASYNC_SCRUB=0` restores the synchronous walk.

  **`supersedeInFlightRequests()` deliberately does NOT tell the worker.** It fires on every pointer move, and the shuttle's target is not the pointer — it is the next frame after the one on screen, which does not move as the pointer travels. Wiring it through was measured: **111 abandoned walks and 141 stale results out of 404 posted**, seeks 28 → 118, cache hits halved, a third of the frames painted. The one case where a pointer move may invalidate work in flight is a reversal, and the test is narrow: not "the pointer moved" but **"the pointer is now on the other side of the picture"**.

  **Cooperative cancellation lives at the top of `decodeUntilTarget`'s outer loop** and nowhere else — the only point inside the walk where no `AVPacket` is owned, `impl_->frame` is not being written, the codec is between send and receive and no conversion is running. It is reached once per packet. `decodeUntilTarget` returns three states, because "the frame is not there" and "the frame is no longer wanted" are different and only the first may take the recovery seek; running it on an abandonment would move `recov` off 0.

  **Cancellation is rarer than it looks like it should be, and that is correct.** One request is in flight at a time and the target only changes on a reversal, so most drags on most files supersede nothing — the in-flight window is one frame wide. `drop` near 0 on light media is not an alarm; `drop` at 0 on **ProRes 4444** is, because that is the file that lags far enough behind the pointer for a release or reversal to land inside a decode.
- **The active drag preview may skip frames on all-intra media, and only there** (Aug 2026, `77738f0` + `f08f015`, plan §15). This is the one place Trace's "never skip a frame" rule is deliberately not in force, and the boundary is exact: **active drag preview only**. Release, stepping, playback and every exact request are untouched. Verified on 4444 with sampling running through the whole drag: landed 133, `delta 0`, three rounds of Right×5/Left×5 return to 133.

  The measurement that justified it split scrub lag into two unrelated causes. **ProRes is a pure throughput deficit**: 4444 measured `ptr 272 f/s` against `dec 52 f/s` — supply 19% — with `walk max 0f` and `rev-hit 0.0%`, because every frame is a keyframe, a seek lands on the target, no intermediate frames are ever produced and there is nothing to cache. Prefetch cannot touch that; prefetching decodes the same frames earlier, not faster. **H.264 is miss cost, not per-frame cost**: forward runs 210 f/s and ends `behind 0`, backward runs 90 f/s on the same frames with `walk max 29f`. Two causes, two mechanisms.

  Stride = (pointer f/s ÷ decoder f/s) × 1.25, clamped, never past the pointer. Results: **4444 p2p 729 → 22ms, max lag 201 → 32f; 422 HQ p2p 478 → 7ms**.

  **The gate is `AV_CODEC_PROP_INTRA_ONLY`, asked of the codec, and three inferred versions were measured wrong first.** Without a gate, sampling on long-GOP is catastrophic — adjacent backward steps are cache hits inside an already-walked GOP (~0.5ms) while a strided step leaves that run and pays a seek plus a fresh walk (~46ms), so skipping raises cost per frame more than it lowers frame count: 4K H.264 backward measured hits 85.4% → 13.3%, decode 90.0 → 13.9 f/s, 76 paints → 14. And it runs away, since a higher stride lowers the measured rate which raises the stride (1080p reached stride 14). The failed inferences, all of which look reasonable: a **latch** on the first observed walk (ProRes seeks land short occasionally; one walk killed sampling for the session, 22ms → 1784ms); a **decaying mean** (collapses during a run of cache hits and declares long-GOP free); a **mean per request** (diluted by forward steps that never seek, so any mixed gesture opened the gate on its backward stretches, stalls 2 of 437 → 13 of 199); a **mean per seek with an evidence threshold** (a forward ProRes sweep performs two seeks total and never reaches it). `ra-walk` is kept in the HUD as the empirical *check* on the codec's answer, not as the answer: 0.00 frames/seek on ProRes, 10–16 on H.264.

  **Two estimator choices are load-bearing.** Capacity is frames *presented* per second over the gesture, not an EMA of per-request cost — the EMA is dominated by whichever of hits and misses came last, reading 0.17ms on a file whose true mixed cost is ~5ms, which collapses the stride exactly when a heavy stretch begins. Presented-per-second is near-invariant under striding, because one presented frame costs one decode whatever the stride. Demand is short-window, because a drag that starts slow and then whips must raise the stride now.
- **`stalls` is measured against the DISPLAY; `hitch` is the number to quote** (Aug 2026, `177759f`, plan §26.1). `stalls` counts paint gaps over `2 × refresh interval` — **8.3ms on this box's 239.999Hz mode and 33.3ms on the 60Hz mode it was also observed in on the same day**. Nothing in the HUD said so, and no stall figure recorded anywhere in this repo is tagged with a refresh rate. Measured on **one run**, 4K H.264 reversals at `win 1284x1067`: `stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. Same paints, same build — 51 or 3 depending only on the threshold.

  **That is most of the "2 of 394 → 44 of 375" mystery** §21.4 carried and §22.8 closed as window size plus machine state. Window size is real and its sweep stands; but §22.8 *recorded* the display changing to 5120x1440 @ 239Hz and filed it under machine state when it was the metric's own denominator. `2 of 394` is what this distribution looks like at a 33.3ms bar.

  `hitch` was **added**, not substituted: `stalls` is "slower than the panel could have shown it" and pairs with `wasted`; `hitch` is "the picture visibly stopped". `stalls` prints its own threshold now. **Third instance of the same failure** — GATE E's `jitter` read 34ms on a schedule within 1.8ms of its deadline. Check what a number is measured *against* before believing it.
- **The reverse-cache budget is 384MB, and drag hitches were cache misses** (Aug 2026, `ac3ae21`, plan §26.3). Reversal drags at `win 1284x1067` on d3d11, 192 → 384MB: 1080p H.264 `hitch 8,8 → 3,2` with `seeks 11 → 4,3` and hit 96.8 → 98.9%; 4K H.264 `hitch 3 → 1,1`, worst gap **169.6 → 80/91ms**; 4444 `hitch 7 → 5`, worst gap 169.4 → 47.9ms. 768MB was measured and is past the knee. **Cost is memory and only memory** — working set 396 → 598MB at 1080p, 677 → 902MB at 4K H.264. `TRACE_REVERSE_CACHE_MB` is the control and the fallback.

  **The footprint is APPROVED, the behaviour is verified, and the FEEL is signed off** (owner, 2026-08-10, plan §26.5 and §26.6 — the subjective scrub test on the shipping build passed): bounded (six consecutive multi-gesture scrub runs plateau at 907–928MB; the HUD reads 382.2 of 384MB after **1357 inserts and 1245 evictions**, never over), discarded on a file change (`close()` clears it and `open()` calls `close()` first — measured 920 → 254MB working set, cache back to `1/129`), and playback-neutral (identical presented rate, frame count, doubling bucket and `handler>budget` on 1080p H.264, 4K H.264 and 4444, at both budgets, two runs each). **Adaptive caching and convert-pool changes were explicitly declined** — don't add them off the back of this.

  **4444 moves least and that is structural**: every frame is a keyframe, a seek lands on the target, no intermediate frames exist, so there is nothing to cache. Don't try to fix its hit rate with more bytes.

  **§15.5 item 1 — "convert Step and cache-fill conversions to display size" — is ANSWERED, and the answer is no.** GATE C already collected it: a full-res 1080p entry is a `yuv420p` plane set at **3.11MB**, not an 8.29MB BGRA frame, so depth was already 64 entries and the hit rate 96.8%, not the weak case that note describes. Display-size conversion adds 3.11 → 2.54MB — eighteen percent — and pays for it by replacing a 0.25ms plane copy with a multi-millisecond swscale resample on the one path whose whole cost is the round trip. **A deferred item's premise expires; re-derive it before building it.**
- **The seek-walk cache fill budget is 240ms, not 60** (Aug 2026, `f08f015`) — **but the SHIPPING default is 60, and this entry is wrong about what runs** (found 2026-08-10 while measuring reverse). The member initialiser is `240.0`, and `open()` then overwrites it unconditionally with `envInt("TRACE_SCRUB_FILL_MS", 60)`, on every media open. So the change below is not in force. **Measured before reporting, and it does not matter on this gesture**: 4K H.264 backward drag, default vs `TRACE_SCRUB_FILL_MS=240` — `rev-hit 94.0 vs 94.2%`, `seeks 6 vs 6`, `ins 124 vs 124`, `hitch 4 vs 4`, `stalls 59 vs 58 of 115`. Treat it as a documentation-versus-code discrepancy rather than a regression to chase: the figures below were not reproducible today, so **correct the note rather than the default**, and re-measure before doing either. The reasoning that follows still stands and is what the reverse fill window is argued from: On the worker it costs the UI thread nothing (`ui gap` unchanged), so the trade that set it no longer applies — **step 5 is what unlocked this, it is not a tuning tweak**. 4K H.264 backward: hit 86.4 → 91.9%, decode 91.5 → 121.3 f/s, seeks 10 → 7, stalls 7 of 73 → 5 of 97. Flat past 240. Memory unaffected (eviction is by bytes against the same budget). 1080p gains little because nothing is halved there, so 24 full-res entries fill the budget and bytes bind rather than time.

  **This is the honest answer to "directional prefetch".** The worker has no idle time on H.264 backward to prefetch *with* — supply is 59–74%, it is saturated whenever there is lag — so speculative lookahead has nothing to spend. What it can do is spend the time it is already using more productively.
- **Scrubbing interrupts playback; it does not end it — and the flag is intent, not state** (Aug 2026, `473b90e`, step 5.6): `sliderPressed` and `valueChanged` both paused unconditionally and nothing in `sliderReleased` restored, so a drag during playback stopped it for good. `userPlayIntent_` means *the user has asked for playback and has not asked for it to stop*, as distinct from `playTimer_.isActive()`, which is whether the mechanism is running. The scrub path suspends the mechanism and **never writes the intent**; the release restores iff it is set.

  **Do not "simplify" this into a snapshot taken when the drag begins.** With `SH_Slider_AbsoluteSetButtons` in force (`9a214f2`), a groove click sets the slider value *before* QSlider emits `sliderPressed` — so by the time a capture there could run, the `valueChanged` lambda has already paused, and it would record "was paused" for a click that began during playback. Gating on `isSliderDown()` fails identically. Carrying an intent makes the emission order irrelevant, and makes a Play or Pause pressed *while the release is still resolving* win by construction: the landing blocks the UI thread, so the keypress is delivered after it, and the restore reads the intent.

  Intent is cleared wherever the user asks for something other than 1x forward playback: pause, stepping (buttons and arrows), J, K, L above 1x, opening media, running out of frames. **A wheel notch over the groove is the one route into `valueChanged` that is not part of a drag** — no press, no release, so nothing would ever restore and the intent would outlive the gesture; an event filter classifies it as the stepping gesture it is and lets the event through unchanged.

  Resume runs **after** `flushVideoScrub(true)`, and the ordering is load-bearing: the landing goes through `loadCurrentFrame` → `reclaimDecoder()`, which already bumps the generation *and* tells the worker, so no older preview frame can be painted afterwards — the release needs no second supersede call, and a bare `supersedeInFlightRequests()` would not have worked anyway since it deliberately does not tell the worker. Resuming earlier would also start audio at the *preview* position, because `startAudioForPlayback()` takes its offset from the current frame. Declined on `playbackAtEnd_` (Play owns the rewind, `c3335ec`) and on a storage-stalled landing. `startPlaybackRun()` is extracted so Play and resume share one setup — resume needs every cadence counter reset that Play does, and a resumed run measured from poisoned counters would corrupt step 6's cadence work.

  Validated with a negative control (plan §16.6): the new `lifecycle.ps1 -PlayThroughDrag` reads 0% moved on the pre-fix build and 13–95% after, with `-PausedThroughDrag` at 0.0% on every file. **Run both** — a check that can only report "moving" proves nothing.
- **The downscale is FILTERED in the shader now, and before this every path in Trace was undersampling** (Aug 2026, `f2d6d57`, step 9, plan §28 — **owner sign-off on the picture 2026-08-10**). The D3D11 sampler is a bilinear 2x2 tap over textures with no mips, so at the validation window's 6.4x reduction it read 4 source texels of every 41. Measured against ffmpeg references at the exact drawn size — `area` at position 0, `neighbor` at 1 — **d3d11 read 0.74, cpu 0.73, and the swscale drag preview 0.76.** Three unrelated mechanisms, one number, because a 2x2 tap is a 2x2 tap; the preview's is not luck, `swsFlagsFor(fast)` returns `SWS_FAST_BILINEAR` and that filter measures 0.74 on the same frame.

  **This is why §9's "local contrast between preview and landing is within 0.7%" saw nothing** — they matched *because* they were equally wrong, and local contrast is exactly the statistic aliasing preserves while moving detail around. It also re-reads §20.3/§21.2: CPU and D3D11 agreeing was never evidence either was right, and the GATE B visual sign-off was taken on that comparison.

  Fixed with a box average over the destination pixel's footprint, in **normalised** source coordinates — which is what keeps GATE C's one-shader design: a chroma plane is smaller, so the same uv offset spans proportionally the same area and 4:2:0/4:2:2/4:4:4 still differ in nothing but texture size. Taps are `ceil(ratio/2)` (each tap is itself a 2x2, so N reaches 2N texels) clamped to 4 **because WARP has to run this too** — though note the CI selftest never draws a frame, so the loop has only ever been *compiled* on a software rasteriser, never executed; the cap is a precaution rather than a measured bound. Averaging happens *before* range normalisation and the matrix: both are affine so it is identical and costs one matrix multiply instead of sixteen — **it would not be identical with a tonemap in between**, so BT.2020 work must revisit the order.

  Results: 4444 **0.74 → 0.02** (delta vs area max 46 → **2**), 422 HQ **0.89 → 0.00**. **No measurable cost** — 4444 99.8%/99.8%, 261/261, 0 doubled, `handler>budget 0 of 260`, max gap 44.3/44.8ms against 45.3 before; 4K H.264 99.1% and identical buckets; 4444 reversal `hitch` 5,9 on against 9,7 off. That is what the draw being idle buys (`draw 0.01ms` of 41.67).

  `TRACE_GPU_REDUCE=0` is the control and is **exact, not approximate**: `taps == 1` collapses the loop to one sample at `input.uv`, and the control re-measures 0.74 / mean 1.32 / max 46, the pre-change figures to the digit. It is a **separate knob from `TRACE_PLANAR_UPLOAD`** on purpose — the reduction is in the YUV shader only, so without its own control the planar-vs-BGRA A/B would differ in two ways at once. HUD reads `display WxH filtered xN`; a preview still reads `1:1`.

  **Both owner decisions are now taken (2026-08-10).** The picture is **signed off**, and the drag preview staying at 0.76 is **accepted as-is** — the picture *sharpens* on release where it used to match, and that is fine because previews are previews. What was accepted is the behaviour, **not** a mandate to change the swscale flag; the owner confirmed that reading and gave the reason, which generalises past this one flag: **smooth, responsive scrubbing takes priority over matching final-frame scaling quality during motion.** Treat that as a standing rule for the whole drag path — preview resolution, preview filtering, sampling stride, paint pacing — rather than a ruling on one flag. Fidelity is owed to the frame the user stops on. **The reopen condition is named and is an observation, not a measurement**: the change on release becoming visibly objectionable in normal use. Until then no further work here is wanted. (If it ever is: `swsFlagsFor(fast)` returns `SWS_FAST_BILINEAR`, plain `SWS_BILINEAR` measures −0.20, and previews are the drag path where supply is 19% on 4444 — measure the shuttle rate first.) Separately, `TRACE_RENDERER=cpu` is now the softer picture as well as the slower one, which matters when telling anyone to try it.
- **Full-resolution frames go to the GPU as three planes; scrub previews do NOT** (Aug 2026, `e8566a4`, GATE C, plan §22). The D3D11 backend takes Y/U/V and applies the matrix in the pixel shader. **One shader covers everything**: subsampling is carried by the size of two textures and resolved by the sampler, so 4:2:0/4:2:2/4:4:4 differ in nothing else, and bit depth, range and the 3x3 are constants rather than compiled variants.

  **The range terms are computed at the actual bit depth.** Reusing the 8-bit 16/255 and 128/255 at 10-bit is wrong — black is code 64 of 1023, 0.062561 against 0.062745 — and the error is a lift of the black point across the whole picture, which is exactly the "global gamma/level shift" the colorimetry notes warn a wrong factor produces. Matrices with no exact coefficients (Fcc, Smpte240m) are **declined** by decoder and renderer alike and keep taking swscale, because an approximation there is a colour difference between backends that no A/B could attribute.

  Confirmed against the CPU path at **three depths independently** — 4K H.264 8-bit 0.006% of pixels differing (max Δ3), ProRes 422 HQ 10-bit 0.002% (max Δ3), ProRes 4444 12-bit 0% (max Δ2). A wrong 65535/1023 would have left 10-bit shifted while 8-bit stayed correct, so this is stronger evidence than one test pattern.

  **The win is headroom, not throughput.** Conversion falls 2.5–4.1x (422 HQ `sws 14.58 → 3.52ms`, 4444 `16.97 → 5.60`, 4K H.264 `3.07 → 1.25`) and 4444's per-frame handler goes 35.20 → 25.22ms of a 41.67ms budget — but **presented rate is unchanged at 98.3–99.6% everywhere**, because none of these files was conversion-bound at 24fps. Don't book it as a playback-rate win.

  **Previews staying on swscale is deliberate and was the thing to verify** (plan §20.7): a preview converts straight to the size it will be drawn at, a fiftieth of a full-res frame, so uploading full-res planes for one moves the cost rather than removing it. Measured over three runs each on 4444, decoder throughput is identical (42.0/44.8/46.8 f/s against 45.8/46.0/42.7), and the worst UI-thread block improves **53 → 30ms** because the landing is a plane copy now.
- **`-Reversals` does not guarantee a landing, and comparing release latency across it produced a regression that did not exist** (Aug 2026, plan §22.4a). GATE C was recorded as taking 4444 release latency 6 → 33ms. It did not. Under `-Reversals` the BGRA config ended on `dst RGB32/BGRA 640x360` — *preview* resolution — with `dec 0.00 | sws 0.00`, meaning no decode happened; the planar config landed a full-res frame. The 6ms was not a fast release, it was no release work. Re-run with **`-SnapRelease`**, where all three configs land full-res (`delta 0`, `target 261 shown 261`), planar is the **fastest**: 33.7–46.7ms against 55.4–65.6ms on cpu and d3d11-BGRA. GATE C improved it by ~20ms.

  Two things to carry: **use `-SnapRelease` for anything about the landing**, and **before comparing two numbers, check the two runs did the same work** — `dst`, `dec` and `sws` said they had not, on the same HUD line as the figure being compared.

  **Planar is not always fewer bytes**: 4:4:4 12-bit is 56.6MB of planes against 37.7MB of BGRA. It is still much cheaper because a memcpy is not a colour conversion.

  `TRACE_PLANAR_UPLOAD=0` restores the BGRA path for an A/B. The capability is asked of the **adopted** renderer, never of `TRACE_RENDERER`: a GPU backend that failed to initialize has already been replaced by the CPU one, and telling the decoder to skip swscale for a backend that needs BGRA blanks every frame.
- **A recycling pool shared by two buffer kinds must only evict when full** (Aug 2026, `e8566a4`): the convert pool holds BGRA previews and planar full-res frames at the same time, because one drag produces both. Its eviction pass dropped every unreferenced non-matching entry on *each acquire* — a no-op while BGRA was the only kind, and a thrash the moment there were two, reallocating ~56MB per landing on 4444 and taking the shuttle 7.8 → 18.2ms/frame while every per-frame cost stayed flat. **A policy that is a no-op under one workload can become a thrash under two, and nothing about the first workload predicts it.**
- **`RenderStats::lastDrawSize` is DEVICE pixels, and both backends fit the video rect with one shared expression** (Aug 2026, `58ec879` + `ddb38ca`). Two bugs with one root: the arithmetic existed twice. The CPU path measured and fitted in *logical* pixels while D3D11 used *device* pixels, so at 1.5x DPI they reported `640x360` and `960x540` for the same rectangle and drew to rectangles a fraction of a pixel apart.

  **The recorded explanation for that divergence was wrong and the measurement that produced it was confounded.** It was filed as a filter-quality difference under a 4x downscale, with the puzzle that the difference was *larger* at 4x than at 6x. The window opens at a fixed logical size, so raising the scale factor also enlarges the video band — DPI and downscale ratio moved together. At dpr 1, sweeping ratios 5.6x → 2.28x (4.33x included), the backends are **identical**. Holding the window and varying only the scale factor: **1.00 → 0%, 1.25 → 8.9%, 1.50 → 5.8%, 2.00 → 0%.** Integer ratios agree exactly; fractional ones do not. Sharing the expression took dpr 1.25 to 2.6% and left dpr 1 and 2 untouched, which is where this box runs.

  Three instruments were needed and none existed: `abshift.ps1` (whole-pixel shift search, to tell geometry from filtering), `abscale.ps1` (scale-factor sweep, to break the confound) and `abcontrol.ps1` (**same renderer twice** — the noise floor, which reads exactly 0 and without which no A/B number means anything). The shift search alone would have closed the investigation wrongly: it reported the pictures aligned, because the offset is *sub*-pixel — a parabola fit to its own numbers put it at (−0.25, +0.5).
- **`d3d11` IS THE DEFAULT RENDERER as of 2026-08-10** (owner decision after testing both side by side; plan §25). `TRACE_RENDERER=cpu` is now the control and the escape hatch — **the first thing to try if anything about the picture looks wrong**. The measured case on 4444: doubled frames 1 → **0**, handlers over budget 1 → **0**, worst present gap 62.5 → **45.9ms**, tick jitter 11–14 → **2–3ms**, `sws` 16.6 → **5.6ms**, real time 99.3 → **99.8%**. It is a *headroom* win that GATE E converted into a cadence win, **not** a playback-rate win — §22.3 measured presented rate as unchanged by GATE C because none of these files was conversion-bound at 24fps.

  Two consequences. **Every scrub and playback baseline in the plan was taken on `cpu`** and most are not tagged with a renderer, because there was only one default; they remain valid as records but not as comparisons against a run taken today — re-tag as you re-measure. And **the untested-DPI gaps were the shipping path** — ~~real mixed-monitor DPI has never run (§20.4) and is TABLED by the owner, 2026-08-10, for want of hardware~~ **DONE 2026-08-14: a second display was connected and the whole hardware case ran (§20.4).** The tabling was correct while it lasted, and the reason is worth keeping: `[System.Windows.Forms.Screen]::AllScreens` returned one display and Parsec replaces it rather than adding one, so those transitions were *not executable on this box at all*, which is a stronger statement than "untested". **It found a real bug** — §4's sizing never re-ran on a DPI change, so a window crossing 100% → 150% came off it the wrong shape with the picture pillarboxed. Fixed at `8945894`, harness at `scripts/measure/dpimove.ps1`. **What is still untested is narrower**: three or more displays, scale factors other than 100/150, a scale change made in Settings while Trace is running on that monitor, and display hot-plug. Also note the display mode on this box was observed changing mid-session on 2026-08-10 (5120x1440 @ 239.999Hz in the morning, 1920x1200 @ 60Hz in the afternoon), so never assume a recorded refresh rate or geometry still holds.

  **The cause of those mid-session mode changes is now known: Anj logging in over Parsec** (owner, 2026-08-10). Remote sessions present a virtual display at **1920x1200 @ 60Hz**; the physical panel is 5120x1440 @ 239.999Hz. This is predictable rather than random, and three things follow. **Ask which one a session is on before comparing any number to a record** — `scripts/measure/refresh.ps1` answers it. **Resolution changes with the refresh rate**, so a Parsec run also has a different window geometry, and window size dominates cache depth and stall counts (§22.8) — the two effects arrive together and neither is visible in a bare stall figure. And most importantly: **subjective smoothness and cadence judgements are not valid over Parsec at all.** Parsec captures, re-encodes and re-times the screen, so it imposes its own pacing on top of Trace's; any owner sign-off on playback smoothness, stutter or scrub feel must be taken **at the machine**. Picture-quality checks (colour, sharpness, framing) are also suspect because the stream is lossy. `hitch` is threshold-independent and stays comparable across both, which is exactly why it is the figure to quote.
- **GATE E is PASSED and the playback stutter is gone — owner sign-off 2026-08-09, "wow, Playback is great!"** The detail that matters: asked which build, the owner had **just double-clicked the app**, i.e. the **CPU default with no GPU path involved**. So E1 alone cleared the complaint, and three things follow. The stutter was **cause A, the tick beat, essentially in full** — §23.5 predicted the owner was seeing beat *plus* a cause-B component on `cpu`, and they were not seeing enough of it to matter. **§23.6 (why 4444 specifically) stays open and is now unlikely to be answerable**, because the fault is gone and the evidence with it — do not re-open it speculatively. And the GPU path's remaining edge on 4444 (0 vs 1 doubled frame, 0 vs 1 over-budget handler, 99.8% vs 99.3%) is **real but below the owner's threshold** — an argument for flipping the default, not a requirement. **GATE E step 2 (vsync snapping, the present/decode swap) is NOT built and is stopped by owner decision**; the design is retained unbuilt at plan §24.4–24.6, and its phase source is settled by measurement as `GetFrameStatistics`, d3d11 only.
- **GATE E DID NOT RUN ON THE J-K-L PATH, and playback there decayed quadratically — FIXED** (Aug 2026, plan §29.2–29.3). `playTimer_.start()` had three call sites: `startPlaybackRun()`, `Key_J` (`MainWindow.cpp:3026`) and `Key_L` (`:3055`). Only the first calls `sessionClock_.start()` (`:1493`), and that is the clock the whole GATE E timeline is built on. On a J or L run both guarded reads take their `: 0` branch — `presentEpochNs_ = 0` (`:1550`) and `now = 0` (`:1578`) — so `target = 0 + slot × period` is **always** ahead of `now`, the rephase branch that exists to catch exactly this can never fire, and the armed delay grows by one frame period per tick.

  Cumulative time to N ticks is `period × N(N+1)/2`, so 8 seconds predicts N ≈ 19 and a final interval of **792ms**. Measured `ticks 19 | sched tick 792ms` on three unrelated runs — media- and direction-independent, because no media clock is involved. Reverse on 4K H.264 managed **20 presents in 8s against the control's 111**; `TRACE_DEADLINE_SCHED=0` restores it and is the workaround.

  **Forward L is worse than slow.** Audio keeps real time while video presents at 1.26 fps, so video **skips 35 frames** chasing it (`sync -5825.1ms`) — a "never skip a frame" violation outside the one sanctioned exception. Space on the same file reads `sched tick 39ms`, `jitter -0.84/0.54/1.73`, **99.5% real time**, `skip 0`.

  Two other readings shared this root cause and were not separate bugs: `drift 9223315866031.3ms` (stale `firstPresentNs_`) and `presented -- / 24.000 fps` (`playbackRateClock_` never started). **Calling `startPlaybackRun()` from J and L would have been the wrong fix** — it also calls `startAudioForPlayback()`, and reverse must stay silent, as must L above 1x. The timeline/telemetry half is extracted as **`beginPlaybackTimeline()`** and shared by all three, the way `startPlaybackRun()` was itself extracted at §16. **After: reverse 20 → 111 presents; L forward `skip 35 → 0` and bit-for-bit the Space control; Space, 4444 playback and the drag path all unchanged; both lifecycle gestures pass.**

  **Why it survived:** GATE E was validated on the Play action alone. It moved playback from a free-running timer to a timeline that must be *established*, and every path that started the timer without establishing it kept compiling silently.

  **It was masking the real reverse-playback weakness.** Reverse on 4K H.264 now measures **86.7% of real time**, `handler>budget 11 of 110 (max 111.1ms)`, `p95 123.6ms` — the GOP-walk cost the roadmap describes. Any reverse-playback figure taken between GATE E and 2026-08-10 measured the scheduler fault instead.

- **ACCELERATED FAST-FORWARD HAD THE SAME FAULT AS REVERSE, and it is one shared fault
  rather than four format bugs** (2026-08-10, `dd21fe9`, plan §11b.2). Reported by the owner
  across every format; reproduced before theorising. **ProRes 4444 asked for 2x and delivered
  1.00x**, then asked for 4x and delivered 1.33x — two rungs that looked identical and
  neither of which was the number on the label; 4K H.264 managed 3.97x of 4x. The speed lived
  in the **tick rate** with one frame presented per tick, so achieved speed was capped by
  per-frame decode cost: ceilings 32 f/s on 4444, 95 f/s on 4K H.264. And `jogForward`
  doubled and capped at **4x**, so 5x/10x/30x were unreachable everywhere.

  The fix generalised the shuttle rather than patching the ladder. Achieved forward speed
  after, all sixteen cells measured: **1080p 2.04/5.02/10.3/28.1x · 4K H.264
  2.06/4.86/12.0x · 422 HQ 2.06/5.27/11.3x · 4444 1.89/5.17/10.8x**, at p50 41.7 / p99
  43.2ms with `handler>budget 0` on every rung. **At exactly 1x nothing changed** — that is
  ordinary audio-mastered playback on the validated path, and the shuttle never enters it.

  The forward walk limit is 4 → **48 for long-GOP only**: a forward stride walks from where
  the decoder already is at ~0.9–2.6ms a frame against a ~30ms seek. **Intra-only keeps 4
  and must**, because there a seek lands on the target for the price of one decode.
- **Reverse 30x snaps to the keyframe grid, and the grid is learned from POSITIONS**
  (2026-08-10, `dd21fe9`, plan §11b.3). Owner decision: **accurate 30x at a stable ~15
  presentations/second**, not a smoother picture at a lower speed. A mid-GOP target costs a
  seek plus a walk that buys nothing when only one frame per GOP is shown; snapping removes
  the walk, and the presentation period is scaled by `advance/stride` so the *content* rate
  stays exactly the commanded speed and the *presentation* rate is what gives. 1080p at 30x:
  **`gop 48` learned exactly, p50 66.3 / p99 68.2ms — 15.1 presents/s at a steady 30.2x**,
  against ~20x with p95 166.8ms before.

  **A statistic over a quantity is not the quantity.** The first cut learned the GOP as
  `max(walk) + 1`, which converges *from below* and stopped at 41 on a file whose GOP is 48 —
  so every "snapped" target missed the grid and still walked, while the HUD read `SNAP gop 41`
  and nothing improved. A request for frame T that walked W frames landed on the keyframe at
  `T − W`, which is an **exact position**, and two of them give the spacing exactly. The
  positions were available all along. The grid is *anchored* on an observed keyframe rather
  than taken modulo the spacing, so a file whose first keyframe is not at 0 still snaps onto
  real ones.
- **The reverse shuttle: queue the frames, and let the SPEED be the stride** (2026-08-10,
  `e9fd236`, plan §11a.3). Two mechanisms, and the second is the one to remember. **It is
  bidirectional now** — see the fast-forward entry above.

  **Reverse decode runs on the scrub worker under the same lease, and results are QUEUED
  rather than presented on arrival.** The tick pops one per slot, so a ~130ms GOP walk is
  absorbed by the queue instead of by the picture. What makes this legitimate where §15.3
  declined it for the drag is that **a reverse target is arithmetic** — at stride S the next
  frame is always `lastAsked − S` — so running ahead is not speculation. The drag's worker
  measured 59–74% supply and was saturated; reverse at 1x measures 80–93% idle.

  **The stride is the COMMANDED SPEED, and presentation stays at one frame per source
  period at every speed.** Nothing measured feeds back into the stride, so it cannot run
  away the way three of §15's four failed gate inferences did — every one of those inferred
  a *cost*, and the stride changes the cost. It also makes cadence identical at 1x and 30x
  by construction, which is the "stable, intentional" half of the goal rather than a tuning
  outcome. The ladder is now 1x/2x/5x/10x/30x (`jogReverse`), the interface's speeds.

  Results, % of the demanded speed: 4K H.264 **1x 87.0 → 99.2**, **2x 75.7 → 100.1**, 5x 95,
  10x ~9.8x, 30x ~26x; worst handler **132.6 → 6.3ms** and long cadence gaps gone entirely.
  ProRes 4444 **1x 99.7 → 100.0%** with the handler falling **24.46 → 3.87ms**, and **10x in
  full at 24 presents/s with `starve 0` and p99 42.9ms** — the file that previously reached
  33% of 4x. ProRes is where the stride is the whole mechanism, because there is no GOP.

  **Reaching the head of the file must STOP playback.** Ending the run without stopping left
  `reverseRunActive_` false while the mode stayed `PlayingReverse`, so the next tick took the
  synchronous path *at the shuttle's speed* (period 41.71/30 = 1.39ms) and decoded on the UI
  thread as fast as it could — visible only in the tail of a run, as `sched tick 1ms`.

  **`isVideoScrubActive()` means "the media is a video file", not "a drag is in progress".**
  Guarding the shuttle on it disabled the entire pipeline while every other counter looked
  healthy; `posted 0` on the worker line was the only symptom. Use `scrubbing_`.

  **Open: the keyframe snap for high speeds.** 4K H.264 reaches ~26x because its GOP is 30
  and a stride of 30 lands on keyframes by arithmetic accident; **1080p (GOP 48) reaches
  ~20x of 30x** with `starve 6 of 17`, because every sample falls mid-GOP and pays a walk.
  Snapping to the nearest keyframe at or below the target costs ~30ms instead of ~71ms there.
  It carries an owner question — see the reverse entries in `docs/reverse-shuttle-plan.md` §11a.5.
- **Long-GOP slice-only threading is REFUTED as a reverse lever** (2026-08-10, plan §11a.1).
  The ~30ms seek intercept really is ~11ms pipeline refill — and removing it costs **13ms on
  every walked frame**, which on a 30-frame GOP is +390ms. Reverse 1x measures 91.9 → **73.5%**
  with a worst handler of **565.8ms**, and forward 4K H.264's handler goes 2.66 → **17.99ms**,
  which would not survive the 60fps budget. **Frame threading is what makes the GOP walk
  cheap.** `TRACE_LONGGOP_SLICE_THREADS=1` is retained as the control. Closed question.
- **Reverse playback is BURSTY, not slow — measured 2026-08-10, `docs/reverse-shuttle-plan.md`.**
  The first cross-format reverse measurement in the project, taken with the new
  `scripts/measure/revplay.ps1` (physical panel, d3d11, 384MB cache, `win 1280x829`).
  4K H.264 reverse 1x reproduces §29.3 to the digit (87.0% vs 86.7%), which is the check
  that the harness measures the same thing.

  **Duty cycle is the number to carry**: handler average against the slot it had to fit in
  reads **7% at 1080p 1x and 20% at 4K H.264 1x**, and both files still miss real time
  (95.4% and 87.0%). The work is not large, it is lumpy — eleven ~104ms GOP walks inside
  5.5 seconds that are 80% empty, `p95 118.9ms` against `p50 41.8ms`. **§15.3 declined
  directional prefetch on the explicit condition "do not revisit without first showing
  measured idle worker time coinciding with stalls", and on the reverse path that condition
  is now met by a wide margin.** The reason the answers differ is that a drag's target is
  the pointer and a reverse run's target is arithmetic (`anchor − round(k × S)`), so
  lookahead there is speculation and here it is not.

  **ProRes reverse at 1x is already perfect and that inverts the usual intuition**: 4444
  reads **99.7% of real time, `handler>budget 0 of 216`, cadence 41.7/43.7/44.3ms** —
  every frame is a keyframe, the seek lands on the target, the walk is empty and each frame
  costs one decode of known price. Uniform work gives perfect cadence; the deadline
  scheduler already converts overload into a *stable slower* cadence rather than jitter
  (4444 at 4x reads `p50 31.3 / max 34.2`, a rock-steady 32 f/s at the wrong speed). Do not
  tune one mechanism against both codec families. `rev-hit 0.0%` on ProRes is still not a
  cache failure.

  **The cost model, from a two-point solve with the fill switched off**
  (`TRACE_SEEK_CACHE_WINDOW=0` makes every step a seek plus a walk of uniform length, so
  the mean and max handler are two points on one line): a walked frame costs **2.59ms at 4K
  H.264 / 1.74ms at 1080p**, and the **fixed cost of a keyframe-aligned sample is ~30ms on
  both**. That it does not scale with resolution means it is not pixel work — the seek
  itself measures 5.8–7.7ms, so **20–24ms per seek is unaccounted for, and the candidate is
  the frame-threaded decoder refilling its pipeline after every flush.** That is exactly
  the mechanism the July 2026 note gives for moving intra-only codecs to `FF_THREAD_SLICE`;
  long-GOP kept `FRAME|SLICE` for forward throughput, and reverse is seek-dominated. **It is
  a hypothesis derived from an intercept, not a measurement** — measuring it needs a code
  change and it is the first experiment of the next session, because it moves the handover
  point and it is what decides whether 10x on long-GOP is reachable.

  **Reverse pays 2.3 seeks per GOP where one would do.** `long-gap med 13` on a file whose
  GOP is 30, confirmed frame-by-frame in the seek log. The cause is that the seek-walk cache
  fill for `RequestMode::Playback` is an **18ms conversion budget written for a Step
  landing**, where one frame is wanted and every speculative conversion is delay in front of
  it — while during reverse the frames walked past are precisely the next 29 requests. That
  is the same argument §15.2 made for Scrub and it was never applied to reverse. Measured
  with the window forced open: **87.0 → 93.4% of real time, seeks 11 → 3, handler avg 8.30 →
  4.18ms, long-gap spacing 13 → 30 = exactly the GOP**, at the cost of max handler 103.8 →
  126.1ms. **That trade is only worth taking once the lump is off the UI thread.** More
  cache bytes are *not* the proposal — §26.5 declined that and the fill window is the
  binding term.

  **`reverseCacheCapacity` is stale pre-GATE-C currency and it silently blocked the first
  attempt at that experiment**: it is `384MB / (w × h × 4)`, the BGRA footprint, so it reads
  11 at 4K where planar entries really give 32, and it clamps `TRACE_SEEK_CACHE_WINDOW`.
  Forcing a 30-frame fill at the 384MB default therefore did nothing at all and looked like
  a refuted hypothesis. Fifth instance of "a premise expires", and the first where the
  expired premise is live code rather than a note.

  **`outside` — per-cycle time that is not the handler — is 3.7–15ms and is unattributed.**
  Renderer-independent (a `cpu` control reads 10.08 against d3d11's 9.92 on the same
  gesture) and not the renderer's own paint, which measures 0.01–0.19ms. It caps presented
  frames near ~110/s whatever the decoder does, i.e. ~4.5x at 24fps if every source frame is
  presented. Attribute it before promising any speed.
- **4444 fast drag is NOT short of the owner's ~4x — candidate item 1 is closed by §15** (Aug 2026, plan §29.1). The item read "~2.3x playback against the owner's stated ~4x", which converts a *decoder throughput* figure into a *drag speed* claim. That conversion was only valid while the shuttle presented every frame; velocity-adaptive sampling (`77738f0`/`f08f015`) broke it **two days after the item was written**, and the item was never re-read against it. Both branches of the "product decision" it proposes are already taken — skipping frames on the heaviest media *is* §15, shipped and signed off; running the worker ahead is directional prefetch, measured and declined at §15.3.

  Re-measured on the shipping build: **at ~4x the picture ends exactly on the pointer and never trails more than 6 frames, in both directions** (`behind 0/6f`, `p2p 26ms`), on 52–54% supply — the figure §15.1 predicted. The fast sweep reproduces §15.2's `p2p 22ms` to the digit with max lag better than §15.4's `cpu` record (`0/21f` vs `0/48f`). The throughput fact (~23ms/frame, untouched) is still true; **supply below 100% stopped meaning "behind" when sampling shipped.** Fourth premise-expiry in three sessions, after §26.2, §27 and §28.

- **THE 8K ProRes 4444 XQ FILE IS NOT SOLVED — OWNER REJECTED THE FRAME-DROP RESULT (2026-08-13).
  THE INVESTIGATION WAS CLOSED UNSOLVED ON 2026-08-15** — see the closure entry below and
  `docs/8k-decode-threads-sweep.md`. Everything in this entry still holds; what changed is that
  the remaining levers were measured and none of them reaches real time, so no further work is
  scheduled. **The acceptance bar below is unchanged and unmet.** ~11 visible fps reads as visibly poor playback, and the same file plays perfectly in QuickTime on a *less powerful* macOS machine. **Do not describe it as solved and do not use frame dropping as the answer.** `TRACE_RT_DROP` is an emergency comparison/fallback only. Acceptance is full-quality 24000/1001 with **every frame presented, `drop 0`**, correct colour and alpha, and no regression to scrub, stepping, reverse or exact landing.

  **THE BOTTLENECK IS TRACE'S SHIPPING STACK — ITS FFmpeg BUILD *PLUS* ITS SERIALIZED PIPELINE — NOT SOLELY ITS APPLICATION CODE.** That correction matters because the first write-up said "inside Trace" and pointed at application code alone. Two independent deficits, each measured, and **neither is reachable by scheduling work**:

  **(a) The FFmpeg build.** Decode-only, no conversion/upload/render/seek (`scripts/measure/decbench/`), 7680x4320 ProRes 4444 XQ. **vcpkg/MSVC and a GCC/mingw LGPL build report the SAME avcodec version, 62.28.102**, so this is a pure toolchain comparison with the version held constant: at `thread_count=32`, wall 6.81s vs 5.89s, and **subtracting the serialized demux gives 24.8 vs 29.3 fps of pure decode — GCC is +18%.** Master (avcodec 63.8.100) measures 24.80 fps against n8.1's 24.62, so **the FFmpeg version is worth ~nothing and the toolchain is worth everything.** In Trace, the same swap takes `dec 45.08 -> 38.58ms` and the handler `74.55 -> 69.08ms`.

  **(b) `thread_count = 0` IS NOT "ALL OF THEM".** FFmpeg's automatic count caps at 16 and warns above it, idling half a 32-thread box: vcpkg auto 16.19 fps against t=32 21.29. `TRACE_DECODE_THREADS` is the knob; **the shipping value must be set from the final build, not assumed to be 32.**

  **(c) The pipeline is strictly serial and nothing is waiting.** `dec 38.58 + sws 17.81 + upload 12.24 = handler 69.08ms` against a 41.71ms budget with **`outside 3.59ms`**. Ordinary 1x playback is otherwise clean — `walk 0f`, `seek n=1`, `io play seq 100.0% seek 0`, `recov 0`, `ctx-rebuilds 0`, `drop 0`.

  **NEITHER HALF REACHES 24fps ALONE, and the arithmetic said so before any code was written.** Async decode on the current build is `max(45.1, 30.1)` = 22.2 fps; a faster build in series is 38.6 + 30.1 = 68.7ms = 14.6 fps. **Both** give `max(38.6, 30.1)` = 38.6ms = **25.9 fps**, and overlapping the demux read inside the decode thread takes it to ~31 fps.

- **THE FIRST 8K CEILING FIGURE IN THIS REPO WAS WRONG, AND SO WAS ITS REPLACEMENT'S ATTRIBUTION.** "20.5 fps = 85%, the machine cannot do it" was measured with the winget `ffmpeg.exe` — a **GPL, GCC, statically-linked FFmpeg 9.0** — at the default thread count, against a Trace that links **LGPL MSVC 8.x**. It substituted a different program for the one being asked about. **Ninth stale-instrument finding and the second out-of-process one.** The rule now has two halves: *benchmark the libraries you ship, at the settings you ship* — and **subtract what your harness serializes that the reference overlaps.** The apparent "FFmpeg 9.0 is faster" gap was entirely `av_read_frame` running inline in `decbench` while the ffmpeg CLI demuxes on its own thread; removing it made both GCC builds land on 29.3 fps.

- **THE ASYNC SCRUB CHAIN PAID ONE CROSS-THREAD ROUND TRIP PER FRAME, AND ON CHEAP FRAMES THAT
  WAS THE WHOLE COST — FIXED 2026-08-14 (`be9f7ec` + `8838c26`, record in
  `docs/comfyui-720p-scrub-measurement.md`).** Owner report: poor scrubbing on
  `14_720P_Comfyui_mp4\video_ComfyUI_0000Fly8.mp4`, a ComfyUI/`libx264` export — **and the same
  file scrubs perfectly in QuickTime**, which is what made it an implementation question rather
  than the contract question the 8K plate turned out to be.

  **THE LEADING HYPOTHESIS WAS THE GOP AND IT WAS NOT THE CAUSE.** The file is 1280x720 H.264
  High, `yuv420p` 8-bit, **no B-frames**, **CFR 24.000000** (`24/1`, tb `1/12288`), 361 frames,
  untagged colour, no audio, ~120KB of embedded ComfyUI workflow JSON that costs nothing
  (`streaminfo 5.07ms`). Its GOP really is coarse and irregular — **7 keyframes, gaps
  24/98/77/45/77/34** against a flat 30 on every other H.264 file in the pool — but a 720p
  entry is 1.32MB, so **the 384MB cache holds 291 of the clip's 361 frames** and warm it reads
  the *best* of the three H.264 files: `rev-hit 99.2%`, `seeks 3`, `ra-walk 7.67`, `hitch 1`
  against 1080p's 98.3% / 5 / 18.40 / `hitch 4`. Cold it walks 97 frames exactly as the
  container predicts and the 1080p control **still** hitches more (5 vs 8). Playback was always
  clean: 100.0% x2, `0 of 275`, `drop 0`.

  **THE CAUSE: `flushVideoScrub`'s async branch posted one frame per round trip, and a frame
  here costs 0.12ms against a 3.56ms delivery interval** — 97% of it round trip, with `dec`,
  `sws` and `paint` summing to 0.25ms. The synchronous walk never showed it because one slice
  covered `ceil(gap * kScrubEase)` frames inside a single call. That ease was dropped from the
  async path with the comment *"the chain runs as fast as frames can be decoded"*; **it runs at
  one round trip per frame**, and with the ease gone there was no way to close a gap faster than
  one frame per round trip. **Eleventh premise-expiry, and the first where the expired premise
  is a comment asserting that a removed mechanism was redundant.** It held at 4K (3.87ms a
  frame) and 1080p (0.68) purely because the frame was expensive enough to hide it, and those
  are the two files it was checked on.

  **This file is the pool's only instance of the failing combination**: frame-*densest* (361
  frames, 1.5x the next) and frame-*cheapest* (0.12ms, 6x under 1080p, 32x under 4K). Density
  sets the demand rate, cost decides whether the round trip is amortised, and every other file
  fails one half. **It is a format class, not one file** — a 10-20s 720p/1080p AI export at
  24fps lands squarely in it.

  **The fix is the sync walk's ease, ported.** One `ScrubRequest` covers
  `clamp(ceil(gap * kScrubEase), 1, 4)` **consecutive** frames, bounded by the same 8ms
  `scrubWalkBudgetMs()`. **What is batched is the ASKING, never the showing** — every frame is
  decoded, delivered and presented individually and in order, nothing is sampled and nothing is
  skipped. **A §15 stride above 1 forces batch 1**, because a batch covers consecutive frames
  and a stride means the wanted frames are not consecutive; they also address different
  deficits, and per-frame decode cost on intra-only media is not reachable by amortising a round
  trip. **The budget is what makes heavy media safe with no size- or codec-conditional branch**:
  checked *after* each frame so a batch always delivers one, and at ~23ms a frame ProRes 4444
  exhausts it immediately and reports `batch cap 4 last 1 max 1`.

  **Cap 4, and it is bounded by measurement at both ends.** Effective cost is
  `roundTrip/N + decode`: N=2 is 543 f/s, N=4 is 1020 f/s, against the fastest pointer demand
  measured anywhere in the set (**479 f/s**) — so 2 barely clears it and above 4 the headroom is
  against nothing observed. Below, the conversion pool's floor is `clamp(maxEntries,4,128)+4` =
  **8 slots** on the largest media and a batch holds its frames alive at once, so 4 is half the
  floor and cannot reintroduce a per-frame allocation during a drag.

  **Result, 720p 1.0s forward sweep, three repeats each, `TRACE_SCRUB_BATCH=0` as the control:
  `dec 269/285/269 -> 344/344/343 f/s`, `behind max 93/73/91f -> 6/6/8f`, pointer-to-picture
  max `292/221/293ms -> 26/26/26ms`.** An ~11x reduction, and the async path now equals the
  synchronous walk it was losing to (`p2p 22/23/22ms`) **while keeping the worker's UI-thread
  protection**. Release lands exact on both (`target 360 shown 360 delta 0`, full-res planar)
  and the batched release is *cheaper* — `walk 5f -> 0f`, `release 11.5 -> 0.3ms` — because the
  picture was already at the pointer.

  **TWO BUGS THE BATCH EXPOSED, BOTH AROUND `busy()`, BOTH INVISIBLE AT BATCH 1** because the
  window they open is exactly one frame wide there.

  **(a) A request posted between publish and drain asked for a frame the decoder had passed.**
  The caller derives its next target from the frame last *presented*, so in that window the
  decoder has advanced N frames and `activeScrubFrame_` has not. At batch 1 re-asking was a
  cache hit; at batch 4 it is a **backward move — a seek plus a GOP walk in the middle of a
  forward drag**: `seeks 1 -> 13`, `ra-walk 0.00 -> 44.85`, `ins 371 -> 986`. `busy()` counts
  undelivered results now.

  **(b) That made `revokeLease()` incomplete and it FROZE THE NEXT SHUTTLE RUN.** It dropped the
  pending target but not results already produced, so `busy()` still read true after
  `reclaimDecoder()` handed the decoder back; `startShuttleRun()`'s `pumpShuttleQueue()` returns
  early on `busy()`, so **the new run never posted its first request** and the tick starved —
  and the run-is-over test reads `busy()` too, so it could not even end. It presented as **one
  random "expected moving" case failing per full transitions matrix** (`R -> ffBtn`, then
  `F -> J`, then `F -> rewBtn`) and **every one passed when run in isolation**, which reads
  exactly like harness flake. **A control binary built from `efda50c` is what settled it: 0
  failures in 50 cases against 2 in 50.** Reaching for the control rather than accepting "it
  passes on its own" is the only reason it was found. After: **75 of 75 across three matrices.**

  **Controls flat.** ProRes 4444 `batch max 1`, `ui gap over 16ms 1 of 841` against `1 of 820`,
  `behind 18 vs 20f`, `p2p 67 vs 68ms`, `delta 0` both — for scale the *synchronous* path on
  that file reads **69 of 84** and ends **196 frames** behind, which is what is being protected.
  1080p identical (`behind 9f`, `p2p 49 vs 54ms`). 4K H.264 reversal drag `hitch 1`, `seeks 3`,
  `delta 0` on all four legs. 4K H.264 cadence x3 **100.0%** with `0 of 119`, `drop 0`,
  `rephase 0`; reverse 1x **100.0%** at 114 frames / 4.75s; both lifecycle legs.

- **THE 4K 9:16 SEEDANCE/ComfyUI FILE HAS ONE KEYFRAME FOR ALL 97 FRAMES, AND THAT IS THE WHOLE
  SCRUB REPORT — measured 2026-08-14, `docs/comfyui-4k-hevc-scrub-measurement.md`, nothing
  built.** Owner report: scrubbing poor, playback fine. **Playback measures 100.0% of real time
  ×2** with `handler 3.55/4.43ms` of 41.67 and all 95 gaps in the ~1× bucket, so the split in
  the report is exactly right.

  **It is not the file the handoff described.** `16_4kSeedance_9x16_Comfyui_MP4` is **hevc
  Main 10, yuv420p10le, 2160x3840, B-frames (reorder depth 2), 97 frames, 34.2 Mbps** — not
  `libx264`, not 8-bit, not H.264. And it carries **exactly one keyframe: frame 0.** The whole
  clip is a single GOP, against 5–9 keyframes on every other H.264 file in the pool and 7 on the
  720p ComfyUI export.

  **Every random-access miss therefore decodes from the head of the file, and a walked frame
  costs 8.9ms.** Two-point solve from single clicks (`clickland.ps1`): 28 frames → **261ms**,
  57 frames → **520ms**, so **8.9ms a frame with an ~11ms intercept**. That intercept is a
  result in itself — the reverse-shuttle work costed a keyframe-aligned sample at ~30ms fixed
  and suspected 20–24ms of frame-thread refill; **here the seek and the flush are free and the
  walk is the entire cost.** Confirmed from inside: `dec 552.50ms | walk 82f` on a click at 85%.

  **The walk is synchronous on the UI thread for a click, a step and a landing**, so:
  **click at 30% → 261ms of dead window, at 60% → 431–520ms, at 85% → 585ms**, against **59–94ms
  on 4K H.264** and 19–54ms on the 720p file. **Stepping backward reads `2 3 2 4 2 4 2 2 2 411
  2 2 3 …`** — cheap while the cache covers, then a full re-walk. `TRACE_SEEK_LOG=1` names it:
  `reason=DecoderBehindOrAtTarget requested=54 current=55 lastDecoded=58`.

  **The cache cannot cover it, and the limit is a byte budget rather than a policy**: a full-res
  entry here is **23.7MB**, so 384MB holds **16 frames of 97** (`cache 2/16 (47.8/384 MB)` read
  off the HUD). Preview entries are 1.5–3.6MB and 100+ fit — `rev-hit 90–98%` through a drag —
  but **a preview entry cannot serve a step or a landing**, by design.

  **Forward dragging is clean** (`behind 0/0f`, `p2p 4/8ms`, supply 127%, `hitch 0`, landing
  `delta 0`): a forward drag never leaves the walked run. **The batch fix cannot help and does
  not harm** — `batch cap 4 last 1 max 1` forward, confirmed before anything else was looked at.

  **`TRACE_SCRUB_FILL_MS=240` fixes the drag and not the press**: `hitch 2 → 0`, `smooth max
  480.7 → 4.9ms`, `dec 42.7 → 54.1 f/s`, while `ui gap max` stays at ~440ms. **600 is
  indistinguishable from 240**, so the budget saturates. `TRACE_SEEK_CACHE_WINDOW=16` costs
  **+22ms on a 431ms landing** and moves the next step freeze from step 11 to step 16; 16 is a
  hard ceiling because `entriesThatFit` = 384MB / 23.7MB. The documented discrepancy is
  confirmed in passing: `scrubWalkCacheBudgetMs` initialises to 240.0 and `open()` overwrites it
  with `envInt("TRACE_SCRUB_FILL_MS", 60)`, so **60 ships**.

  **THE HANDOFF'S PROPOSED GENERALISATION DOES NOT SURVIVE, AND THE REPLACEMENT IS A PRODUCT OF
  TWO TERMS.** It proposed one class — a default `libx264` keyint, with resolution deciding
  whether the cache covers a GOP. The encoder half is wrong and the cache half is not what bit.
  What predicts the failure is **(frames back to the previous keyframe) × (per-frame decode
  cost)**: 720p 98 × 0.36ms = **35ms** (harmless, its fault was the round trip); 4K H.264
  29 × 2.8ms = **81ms** (the recorded 90–125ms press landing); **this file 96 × 8.9ms = 855ms.**
  **A keyframe count alone would have condemned the 720p file equally.** The 720p control makes
  it legible: it walks 64 frames on one click for **23ms**, and caches **all 64** of them
  (`64cv/15.40ms`) where this file caches **3 of 82** — same 18ms budget, conversions 0.24ms
  against 5.07ms.

  **Do not reach for sampling** (§15's gate is `AV_CODEC_PROP_INTRA_ONLY` and HEVC is not; one
  keyframe makes strided steps worse, not better) **or for more cache bytes** (§26.5, 768MB past
  the knee). Options ranked in the doc, **none built**: the exact landing off the UI thread
  (roadmap 2b), a fill budget proportional to the walk it is keeping instead of a constant 18ms,
  and `skip_frame = AVDISCARD_NONREF` while walking. **The contract question is open and is the
  owner's** — showing frame 57 of a single-GOP file requires decoding 58 frames in any player
  ever written, so a player that feels instant is either alive during the wait or showing an
  approximate frame.

  **Three instruments added, and one of them lied first.** `widen.ps1` widens the window without
  changing the video rect — a portrait fit is height-bound, so width is pure letterbox, `display
  460x818` is identical either side, and **this is what makes phase 12's clipped-HUD limitation
  cost nothing on 9:16/4:5/1:1 material**; it is also the only reason `scrub.ps1` runs on this
  file at all, since at the §4 width the groove is under its 300px minimum. `clickland.ps1` and
  `stepcost.ps1` measure a bare click and a bare step, which no drag harness reaches.
  **`clickland.ps1`'s first version sent ONE `SendMessageTimeout` after the click**; a sent
  message is serviced ahead of posted mouse input, so it reported **3ms for a landing that
  blocked for 450** — consistently enough to "show" that `TRACE_SEEK_CACHE_WINDOW=16` removed
  the freeze. It does not. **Eighth instrument to accuse or exonerate a build wrongly, and the
  first where the wrong answer was the flattering one.**

  **OWNER RULING, 2026-08-14: VLC STRUGGLES WITH THIS FILE TOO, so the contract question is
  closed in the useful direction.** Nobody is showing frame 57 of a single-GOP file instantly,
  so there is **no evidence any player is displaying an approximate frame during the gesture**,
  and exactness is not what costs Trace anything here. **Do not weaken the landing.** What is
  left is entirely the synchronous walk on the UI thread — **the fix is to get it off that
  thread, and it costs nothing in exactness.** Option 1 is the accepted one and is scheduled
  **first**, ahead of both decode-queue stages.

- **CHECKPOINT 2's ARITHMETIC PUT CONVERSION ON THE WRONG SIDE, AND ONE QUEUE STAGE CANNOT REACH
  THE 8K TARGET — design at `docs/async-decode-queue-design.md`, 2026-08-14, nothing built.**
  The scoping read *decode 39.68ms · conversion + upload 30.3ms · overlapped `max(39.7, 30.3)` =
  25.2 fps*. **`convertCurrentFrame` is called from inside `decodeFrameAt`**
  (`VideoDecoderFFmpeg.cpp:2185`, `:2200`) and the worker's only decoder call **is**
  `decodeFrameAt` (`ScrubDecodeWorker.cpp:217`) — which is already visible in shipping
  behaviour, because a drag's `sws` figure comes off the worker's own perf snapshot. **Moving
  decode to a worker moves conversion with it.**

  Measured this session, 8K plate, `TRACE_RT_DROP=0`, vcpkg build: **`dec 49.33 + sws 17.02 +
  upload 12.10 = 78.45` against `handler 78.25`, `outside 2.08ms`** — strictly serial, nothing
  waiting, `handler>budget 88 of 88`, 51.6% of real time. So one stage gives
  **`max(56.7, 13.2)` = 17.6 fps** on the minimal FFmpeg build against 14.4 today: **+22%, and
  short of the 24 the file is accepted at.** Building against 25.2 would have produced a correct
  implementation that measured as a failure.

  **The 25.2 figure is reachable and needs a design nobody has described**: conversion in a
  *second* stage, decode N+2 while converting N+1 while uploading N. That changes the decoder's
  output boundary (`VideoFrame` is post-conversion, and `VideoFrame.h` admits no FFmpeg type
  because the image-sequence path must compile without FFmpeg), and both stages then contend for
  the cores each was measured with alone — against a **~2ms per frame** margin. **Owner
  decision.** The rest of the design stands and is unaffected: a bounded lookahead **cache** and
  never a schedule (the tick still picks the frame; `cd79d49` is the scar), draining
  **inside `reclaimDecoder()`** so cancellation is one choke point rather than an enumeration,
  byte-bounded shallow depth justified by measured starvation, default off.

  **OWNER RULING, 2026-08-14: FUND THE TWO-STAGE PIPELINE — the 8K plate is wanted close to real
  time, so 17.6 fps is not the destination. BUILD THE SINGLE STAGE FIRST ANYWAY**, because it is
  real headroom everywhere, it is the prerequisite for two, and it is the honest way to find out
  whether the second stage's ~2ms/frame margin survives two stages contending for the cores each
  was measured with alone. **Report after stage one, before starting stage two.**

  **Carried, and it is a real signal rather than a puzzle:** the owner observes that *scrubbing*
  the 8K plate feels better than playing it. The candidate explanation is that they are not doing
  the same work per presented frame — a scrub preview converts to the **display size** rather
  than full resolution, ProRes is intra-only so a seek **lands on the target** with no walk, and
  **§15's sampling is active on intra-only media** so a fast drag legitimately skips source
  frames — against playback's every frame, full resolution, strictly serial 78.45ms. **Confirm it
  from the HUD rather than asserting it** (`dst` and the preview size, `sample stride`,
  sampled-vs-presented on a fast 8K drag, against the playback figures); it is the clearest
  available statement of what the pipeline has to buy back.

- **THE 8K ProRes 4444 XQ INVESTIGATION IS CLOSED (owner, 2026-08-15).** Not solved — closed.
  **Best full-quality decode and display is 13.64 fps = 56.9% of real time** (minimal FFmpeg
  build, default threads, `drop 0`, every frame presented), or **62.0% with stage one at depth
  2**. **Decode is the binding term**: `dec 39.08ms` against a `41.71ms` budget, 94% of the whole
  frame before conversion or upload, and **flat in thread count from 32 to 64**. **Stage two is
  not justified by the measured margin** — a perfect three-stage pipeline ceilings at
  `max(39.08, 18.01, ~13.5)` = **25.6 fps, a 6% margin over 23.976**, against contention stage
  one measured at **+24% on `sws` and +91% on `upload`**; a 10% inflation of decode puts it below
  24. **`TRACE_RT_DROP` remains an EMERGENCY FALLBACK and never the answer** — the owner rejected
  the frame-drop outcome on 2026-08-13 and the acceptance bar is unchanged and unmet (full
  quality, every frame presented, sustained 24000/1001, `drop 0`, `hitch 0`). **Do not begin
  stage two, and do not begin CUDA/NVDEC work.**

- **STANDALONE DECODE THROUGHPUT IS NOT PRESENTATION THROUGHPUT, and three numbers for this file
  differ by 2x.** `decbench` standalone on the minimal build at t=32 reads **25.24 fps** (decode
  only, demux subtracted); Trace's own in-process `dec` term reads **39.08ms = 25.6 fps
  equivalent**; Trace's **presentation throughput is 13.64 fps**. **The first two agreeing within
  1.5% is the useful result** — Trace's decoder is not slower than a standalone harness, and the
  entire gap to 13.64 is the rest of the pipeline being serial. Two recorded instances of getting
  this wrong: the first 8K ceiling figure here (**"20.5 fps"**) was the winget GPL/GCC
  `ffmpeg.exe` at default threads substituted for the LGPL/MSVC libraries Trace links, and
  `ffmpeg -f null` made slice-only threading look *faster* on every ProRes file because it
  decodes and discards, so there is nothing for a frame-threaded decoder to overlap with. **A
  benchmark that removes the work your program does around the thing being measured is measuring
  a different program.**

- **DECODE THREADING IS ALREADY AT ITS KNEE BY DEFAULT, AND THE NOTE THAT SAID OTHERWISE WAS
  WRITTEN LAST SESSION (2026-08-15, `docs/8k-decode-threads-sweep.md`).** The stage-one report
  recommended raising `TRACE_DECODE_THREADS` as "the cheapest remaining lever, still at FFmpeg's
  automatic count, which caps at 16". **It has been `av_cpu_count()` for intra-only since
  checkpoint 1** — 32 on this 16-core/32-thread box — and the "+21%" it quoted was measured
  before that default landed. **Thirteenth premise expiry, and the first written by this
  project's own previous session.** The guard is now in the product: the HUD reads **`thr slice
  x32` / `thr frame x16`**, taken off the codec context rather than recomputed from the env, so
  "the default is N" is an observation. It earned itself on its first run by confirming 4K H.264
  is still `frame x16`, i.e. the long-GOP split is live.

  **The knee is the logical CPU count and the curve is FLAT beyond it to 64.** 8K plate, vcpkg,
  `dec` avg: t=1 **661.23** · 8 98.14 · 16 62.29 · 20 56.90 · 24 52.42 · 28 48.88 · **32 45.95** ·
  40 45.11 · 48 45.10 · 64 45.12. Minimal GCC FFmpeg: 16 55.53 · 24 46.10 · **32 39.08** · 40
  39.19. **CPU never exceeds ~50% of the machine (15.8 of 32 cores) at any setting from 32 up**,
  so the box is neither thread-starved nor saturated — past 16 threads you are on SMT siblings for
  ~26%, and past 32 there is nothing left to ask for. Amdahl on t=1/t=32 gives a **3.9% serial
  fraction** whose asymptote is ~26ms against a measured floor of 45ms, so **the limit is per-core
  throughput and memory traffic, not parallelism.**

  **DECODE ALONE CANNOT REACH 23.976 AND IS NOT CLOSE.** At the knee on the fastest build `dec` is
  **39.08ms against a 41.71ms budget — 94% of the whole frame** before conversion (18.01), upload
  (12.33) or paint. Ceiling if everything else were free: **25.6 fps.** Best measured throughput
  **13.64 fps / 56.9%** (minimal build, default threads, `drop 0`); with stage one at depth 2,
  **62.0%**. In the shipping drop-enabled config, `presented 13.46`, **`drop 62`, `media 98.1%`**.

  **KEEP THE DEFAULT POLICY EXACTLY AS IT SHIPS** — `av_cpu_count()` for intra-only, FFmpeg's
  automatic for long-GOP. It is derived from the machine rather than hard-coded, so a four-core
  box gets 4 where a literal 32 would break it. **The raised count also HELPS random access**,
  which is worth recording because the opposite was plausible: 4444 `scrub -SnapRelease` at
  default against `TRACE_DECODE_THREADS=8` reads shuttle **29.63 → 15.89ms/f**, `hitch` **2 → 0**,
  paints **48 → 84**, `delta 0` and full-res planar on both. Slice threading makes `thread_count`
  threads *per frame*, so a seek needs no pipeline refill and there is no scrub penalty to trade.

  **STAGE TWO IS NOT JUSTIFIED FOR THE PURPOSE IT WAS FUNDED FOR, on these numbers.** A *perfect*
  two-stage pipeline gives `max(39.08, 18.01, ~13.5)` = **25.6 fps at zero contention**, and stage
  one measured the contention that arrangement meets (`sws` +24%, `upload` +91%) against 2.4ms of
  margin. Realistic landing zone is **~22–25 fps, below 24 in the likely case**, in exchange for
  changing the decoder's output boundary. What is left is outside both stages: closing a 1.6x gap
  on ProRes 4444 XQ needs a faster decoder, and hardware decode is excluded by the owner. **The
  honest position is that this file is not reachable at 23.976 on this machine with this decoder**
  — the same conclusion `ffmpeg -f null` reached independently at 20.5 fps with every other stage
  deleted.

- **THE EXACT LANDING IS OFF THE UI THREAD — a click, a release and a frame step no longer
  freeze the window (2026-08-14, `cc8e638`).** Owner ruling of 2026-08-14: VLC struggles with
  the single-GOP Seedance clip too, so no player is showing an approximate frame during that
  gesture and **exactness is not what costs Trace anything there — do not weaken the landing.**
  What cost it is that showing frame 57 of a 97-frame one-keyframe file requires decoding 58
  frames at 8.9ms each, and Trace did all 58 inside the mouse handler.

  The request is unchanged — `RequestMode::Step`, one frame, full resolution, accurate
  conversion, `batch 1` and **`batchBudgetMs` deliberately 0**, because a budget on one frame
  could only mean "give up". Only the thread changed. Measured on the Seedance clip with
  **`TRACE_ASYNC_LANDING=0` as the in-binary control**: click at 30/60/85% **267/424/589 → 0/0/0ms
  frozen**; backward step x16 **max 410 avg 29 → max 31 avg 14ms**; forward drag **`ui gap max`
  227.7 → 8.3ms** and `over 16ms 1 of 1139 → 0 of 1516`. **`release` 597.7 → 595.2ms — the walk
  is the same walk.** This makes 520ms not a freeze; it does not make it shorter.

  **Three faults the first cut had, each found by measuring.** **(a) A CLICK DECODED THE SAME
  FRAME TWICE.** A click is press+release on the same value; synchronously the press landed
  before the release arrived so the release took `scrubShownExact_`'s skip, but with the landing
  on the worker the release arrives mid-walk and re-posting *cancels* the walk and restarts it
  from the head — `sup 1`, `cancel max 128.43ms` at a `ckpt 135.37ms` granularity, picture at
  570ms against the synchronous 555. `requestExactFrameAsync` now **adopts** an in-flight landing
  for the same frame. **(b) A MID-DRAG FLUSH RE-FROZE THE WINDOW**: `activeScrubFrame_` means
  "what is on screen", so it is still -1 while the press landing walks, and a pointer move inside
  that window failed the drag branch's `walkFrom >= 0` test and fell through to the synchronous
  block. `flushVideoScrub` defers while a landing is pending unless this is the release, which
  must always be able to move the target. **(c) `release` WOULD HAVE READ 0.1ms AND FLATTERED THE
  BUILD** — it is set from the landing's own post-to-on-screen clock now, so it keeps meaning
  "how long until the exact frame appeared". **The win belongs to `ui gap max`, a separate field
  because it is a separate claim.**

  **RAPID STEPS NOW COALESCE, and that is a stated behaviour change rather than a side effect.**
  Each press advances the playhead and supersedes the landing in flight, so five fast presses
  move five frames and decode the fifth. The arithmetic is identical and the frame the user stops
  on is exact — `lifecycle -StepCycle` lands on frame 62 and returns to frame 62 through
  3 x (Right x5 / Left x5), with `landing async 32 sync 1` showing all 30 steps landing
  individually at 120ms spacing. What it replaces is worse by every measure: held arrow keys used
  to queue one full decode per press and run the playhead away from the user after they let go.
  **HUD `landing PENDING|idle async N sync N sup N (Xms max Yms)`** makes a silent fallback to the
  synchronous path visible. Note **`display` after a release now reports the LANDED frame** rather
  than the previous preview, because the async landing repaints explicitly where `loadCurrentFrame`
  only called `update()` — a HUD-accuracy change, not a regression.

- **CHECKPOINT 2 STAGE ONE IS BUILT, DEFAULT OFF, AND MEASURED — +10%, NOT +22%, AND THE REASON
  IS THE STAGE-TWO DECISION (2026-08-14, `d8beba8`; report in
  `docs/async-decode-queue-stage-one.md`).** `TRACE_PLAYBACK_QUEUE=N`, **default 0**. A bounded
  lookahead **cache and never a schedule**: the tick's target arithmetic is untouched, a starve
  **holds and counts** rather than taking the decoder back, and draining lives **inside
  `reclaimDecoder()`** so every transition drains it by construction.

  8K plate, `TRACE_RT_DROP=0`: vcpkg **off 53.6% · d1 48.3% · d2 58.2% · d3 clamps to 2**;
  minimal GCC FFmpeg **off 56.5% · d2 62.0%**. **Depth 1 is WORSE than off** because a depth-1
  queue cannot overlap — the worker only starts N+1 after the tick consumes N — so **depth 2 is
  the minimum that overlaps**, and the byte bound already caps 8K there (512MB / ~199MB).
  **Overlap is confirmed on the design's own terms rather than on the frame rate**: `handler`
  70.42 → 16.28 while `dec` stayed 38.55 → 41.08 and `outside` rose 3.13 → 28.32.

  **WHY IT IS +10%: both overlapped stages get SLOWER concurrently** — `sws` +24%, `upload` +91%.
  The binding term is the worker's own serial `dec + sws`, because **conversion rides with
  decode**; `41.08 + 22.74 = 63.8ms = 15.7 fps` predicted against 14.87 measured. **The design
  flagged contention as a stage-two risk against a ~2ms/frame margin and it is already material
  at ONE stage** — so stage two, which would take the worker to `dec` alone at 41.08ms = 24.3 fps,
  lands *near but under* 24 rather than at it. **`dec` is the binding term in every arrangement
  and is the one thing neither stage touches**; `TRACE_DECODE_THREADS` is already known to be
  worth +21% here and still sits at FFmpeg's automatic count, which caps at 16 on a 32-thread box.
  Recorded as a recommendation, not taken.

  **Two faults found by measuring.** **The playhead advanced on a starve, and that is a runaway** —
  the target ran ahead at 24 fps while the pipeline supplied 20, so every arriving frame was
  already behind and was discarded: `posted 94, drop 93, starve 146, reseed 50`, **one frame in
  6.14s, 0.7% of real time.** A starve is not a failure and now leaves the playhead where it was,
  exactly as an audio hold does. And **`wait` was measuring the upload and read 52.01ms on a run
  where nothing had waited** — `setFrame()` is where the 24.58ms D3D11 upload happens; timed to
  the queue decision only it reads **0.01ms**. Ninth stale instrument, and the second in two
  sessions where the wrong answer was the alarming one rather than the flattering one.

- **WHY THE 8K PLATE SCRUBS BETTER THAN IT PLAYS — confirmed 2026-08-14,
  `docs/8k-scrub-vs-playback.md`.** The owner raised it and two of the four predicted reasons
  needed correcting. **Confirmed:** the preview converts to the **display size**
  (`dst RGB32/BGRA 1090x614` against full-res `YUV444P12 planar`), taking `sws+upload` from 28.91
  to 13.05ms and the frame from 82.55 to 52.72. **Corrected:** "intra-only, so a seek lands on the
  target" is true and is **not a difference versus playback on this file** — both read `walk 0f`;
  and sampling is gated on but **only engages on demand** — a slow drag reads `sample idle |
  stride 1 | skipped 0`, only a 0.4s sweep reads `sample ON | stride 3 | skipped 92 over 7 steps`.
  **THE DECISIVE ONE WAS UNSTATED: a drag has no deadline and playback does.** The decoder
  supplies the same ~13 fps either way — `presented 12.86` in playback, `dec 13.0` slow drag,
  `dec 13.6` fast drag. Playback is judged against 23.976 and misses on **every** frame
  (`handler>budget 143 of 143`, 53.6%); a drag is judged against the pointer, which asked for
  13.8 f/s and was **met** (`supply 94%`, `behind 0/1f`) or asked for 163.9 f/s and was
  legitimately allowed to trail and sample (`supply 8%`, `behind 0/90f`) with the landing still
  exact. **Scrubbing feels better because nothing promised it 24.** What the pipeline has to buy
  back is therefore **13 → 24 fps at full resolution**, and the drag shows which lever moves which
  term: the preview cuts `sws+upload` 44x while `dec` does not move at all.

- **MIXED-MONITOR DPI IS VALIDATED ON HARDWARE AND §4 NEVER RE-SHAPED ON A SCALE-FACTOR CHANGE — FOUND AND FIXED 2026-08-14 (`8945894`), plan §20.4.** A second display was connected, closing the item that had been **tabled for want of hardware** since 2026-08-09 and was the named beta gate. Configuration: `\\.\DISPLAY1` 5120x1440 @ 239.999Hz at **100%**, `\\.\DISPLAY2` 1920x1080 @ 60Hz at **150%** — different scale factor, different refresh rate, different work area.

  **The bug.** §4 asks the window to *"recalculate correctly when the window moves between monitors with different scaling"*. Nothing did. **A DPI change is not a `QEvent::WindowStateChange`**, so `changeEvent`'s re-shape never ran; and **it is not a drag**, so it sends no `WM_SIZING` and `constrainSizingRect`'s aspect lock never ran either. Windows scaled the rect on its own and Trace accepted the result. Measured on 4K H.264 crossing 100% → 150%: the client lost **147 logical px of height** (1083 → 936) while the width was preserved exactly, so **the picture pillarboxed** — `display 1201x676` filling the viewer at open became `display 936x527` inside an unchanged `win 1201x934` after a round trip, and the window was 973 logical tall against a **672**-logical work area, far past §4's 80% rule. **Not cumulative**: four round trips converged on a stable wrong pair. Reproduced with **Win+Shift+Arrow**, so it is not an artifact of how the harness moved the window.

  **The fix is conditional on the SCALE FACTOR, not the monitor.** `reshapeAfterDpiChange()` is armed from `WM_DPICHANGED` through a **200ms coalescing** timer — deferred because at message time Qt has not applied the new dpr or relaid out, and the shaping pass *measures the layout*, so shaping there reads stale terms (the same mistake as shaping before `refreshHud` at open). Two monitors sharing a scale factor need no reshape, and resizing a window because the user dragged it somewhere would be its own surprise. After: the round trip returns to the opening geometry **exactly**, repeatably, both directions.

  **THE SELFTEST PASSED EVERY ROW WHILE THE BUG WAS LIVE, AND THAT IS THE LESSON.** `WindowShape.cpp` was correct throughout — all 11 shapes × 4 scale factors — because the arithmetic was never the problem: **the shipping path never reached it. A pure function cannot notice that nobody called it**, so a green selftest over one says nothing about whether anything invokes it. Eighth stale-instrument finding, and the first where the instrument was *green* rather than accusing.

  **Everything else passed.** D3D11 swapchain resize and the CPU path agree **to the pixel** (`display 960x540 | win 960x1151` on both). Fullscreen on the secondary goes to the right monitor, covers it exactly, and Escape restores the pre-fullscreen rect there. The aspect lock holds through an interactive corner drag **at dpr 1.50** (993/558 = **1.779**) — `constrainSizingRect` works in physical px and every prior run of it was at dpr 1, where a missing dpr term is invisible. Cadence on the **60Hz** secondary is **100.0% of real time, all gaps ~1x, `hitch 0`, `drop 0`** — GATE E's refresh-independence confirmed rather than assumed. Opening media on the secondary computes §4 against that work area (4x5 reads `bound minimum | want 460x575 | got 460x575 (0.8000)`, correctly pushing past the 80% budget but not past the work area). **Crossing while playing costs nothing**: 4444 crossed twice (`dpiChg 2 reshape 2`) and finished at **99.8%, 261 frames, `drop 0`, `0 of 260`, `hitch 0`**.

  **REFINEMENT, 2026-08-15 (unattended session, same hardware, `dpimove.ps1 -Mode fullscreen`):
  "restores the pre-fullscreen rect there" is true of size and monitor, not quite of position.**
  On the secondary (150%) the restored rect reads **7 physical px (≈4.67 logical px) lower in Y**
  than the pre-fullscreen rect, reproduced identically twice (`pre 5774,0 982x1228` →
  `post 5774,-7 982x1228`). The primary (100%) leg is exact (`pre 40,40 1192x1122` → `post 40,40
  1192x1122`), so this is scale-factor-specific, not a general fullscreen-restore fault. Width
  and height are untouched — this is a small position drift, not the framing/size bug the harness
  warns to look for. **Not investigated further and not patched**: the restore goes through Qt's
  own `saveGeometry()`/`restoreGeometry()`, chosen deliberately over a hand-rolled restore
  (`toggleFullscreen`'s own comment: "Qt's own restore is not something to rely on across a window
  manager" — chosen anyway as the more robust option), and a few physical pixels of position drift
  from Qt's internal DPI rounding is a different class of question than the pixel-exact geometry
  math §4 owns. Also confirmed on the same hardware: the round trip
  primary → secondary → primary reproduces exactly (`982x1228`/`1261x818` both directions,
  repeated) **once the window's own chrome is not itself changing mid-sequence** — toggling the
  dev HUD does not trigger a reshape (by design, matching the phase-12 "hiding the HUD moves
  `stalls`, `win WxH` does not" finding), so a round trip that hides the HUD partway through
  correctly lands on a *different, smaller* geometry than it started from, sized for the smaller
  chrome — that is the reshape mechanism working as designed, not a round-trip defect, and was
  confirmed by running a second round trip after the HUD state stopped changing (matched to the
  pixel both times). Maximize on both monitors re-verified clean (fills each work area, stays on
  its own monitor, restore-to-normal returns the media-shaped size). **The remaining "still
  untested" items (three-plus displays, scale factors other than 100/150, an in-session Settings
  scale change, hot-plug) were not attempted this session** — the first three need either
  additional hardware or changing OS display settings, and per this session's own operating rules
  changing system/display settings is not an action to take unsupervised.

  **HUD gains `dpr N scr NAME dpiChg N (a->b) reshape N`.** `dpiChg` counts the messages, `reshape` counts what was done about them — separate fields because they are separate claims, and before the fix the first read 1 and the second would have read 0.

  **THE DEV HUD IS WHAT MAKES THE SECONDARY LOOK BROKEN, AND IT IS PHASE 12'S DIAGNOSTIC LIMITATION AGAIN.** With it shown, chrome reads `0x407` logical against a 672-logical work area — 61% of the monitor — so the window overflows, the fullscreen-Escape restore lands 7px high, and the corner-drag grab point falls off screen. **With `H` pressed (the shipping configuration) every one of those is clean**: the window fits at 545 logical, Escape restores exactly, and the 4K clip lands at a **1280x720** viewer, §4's area cap to the digit. Do not report any of them as product defects.

  **TWO HARNESS TRAPS THIS CONFIGURATION CREATES FOR EVERY OTHER SCRIPT.** **(a) Windows launches a default-positioned window on the monitor the MOUSE CURSOR is on**, so a fresh `Trace.exe` opens wherever the last gesture left the pointer. A regression run after any pointer work on the secondary is taken at 60Hz, 150% and the viewer minimum — `display 960x540` against the baseline's `1226x690` — and window size dominates cache depth and stall counts (§22.8) while refresh sets the `stalls` threshold. It reads as a regression and is not one: **park the cursor on the primary before quoting any figure, and quote `scr`.** **(b) `powershell.exe` is SYSTEM-DPI aware**, so every harness in `scripts/measure/` virtualizes rects and captures for a window on the secondary. The first probe written for this pass reported **both displays at 100%** while one was at 150%, because a 1920x1080 panel at 150% reports as 1280x720 at 96 DPI under system awareness. `dpimove.ps1` sets `PER_MONITOR_AWARE_V2`, asserts it on line one, and **refuses to measure** otherwise.

  **Still untested, and narrower than before**: three or more displays; scale factors other than 100/150 (**125% and 175% are the fractional cases §20.3 cares about**); a scale factor changed in Settings while Trace is running on that monitor; display hot-plug. **§20.3's cross-backend band difference at 150% is NOT closed by this** — that was measured under `QT_SCALE_FACTOR`, and this pass compared *framing* across backends, not the band diff at real 150%.

  **ALL OF THAT RESIDUE IS WITHDRAWN BY OWNER DECISION, 2026-08-15. The second display is disconnected and no multi-display work is to be proposed.** What §20.4 covers stands and is not withdrawn — the hardware pass ran at 100%/150%, found and fixed a real bug, and closed the beta gate. What is withdrawn is the *remaining* list above: 125%/175%, a live scale change, hot-plug, three or more displays, and the band diff at real 150%. **Do not re-propose any of it, and do not list it as an open item in a handoff.** It is recorded here as a known, accepted gap rather than as work.

  If a multi-monitor or fractional-DPI bug report ever arrives from testing, this list is where to start and the hardware has to come back first — `scripts/measure/dpimove.ps1` sets `PER_MONITOR_AWARE_V2` and refuses to measure without it, so it is ready whenever that happens. Until then it is closed.

- **THE MINIMAL MinGW/GCC LGPL FFmpeg IS BUILT, INTEGRATED AND GREEN IN CI (2026-08-14).** `scripts/build-ffmpeg/build-minimal-ffmpeg.ps1`, selected by the `TRACE_FFMPEG_ROOT` CMake hint. **The vcpkg path is untouched and still builds** — the hint is unset by default and deleting one line of the workflow returns CI to it.

  **Recorded artifact facts.** DLL set **20.8 MB** (vcpkg 17.3, BtbN 104.3). avcodec **62.28.102**, `av_version_info` **8.1.2** — the same sonames as vcpkg, so it is a drop-in swap. Licence **LGPL v2.1 or later**, read back from `avutil_license()` on the built binary; no `--enable-gpl`, no `--enable-version3`, and **zero `--enable-lib*` tokens**. Notices in `scripts/build-ffmpeg/THIRD-PARTY-NOTICES.md`. **Decode thread default: the machine's logical CPU count, intra-only only** (`av_cpu_count()` clamped to 64) — swept on the final build, it knees at 32 on this box (t=28 24.08, **t=32 25.24**, t=48 25.45, t=64 25.32 fps) and long-GOP keeps FFmpeg's automatic count because there `thread_count` is frames in flight, not threads per frame.

  **Pinned, and the pins are the cache key.** winlibs GCC 16.2.0 (mingw-w64 UCRT 14.0.0-r1), NASM 2.16.03, FFmpeg n8.1.2, msys2 2026-06-11 — all SHA256-checked. CI keys the cache on `hashFiles` of the script, so editing a pin invalidates it by construction. **The green run built from scratch** (`Cache not found for input keys: ffmin-…`), ~9 minutes.

  **"Minimal" is vcpkg's feature set built with GCC**, minus `avdevice`/`avfilter` (Trace includes neither header) and minus **encoders and muxers** (Trace decodes and demuxes; there is no encode path in `src/`). Decoders, demuxers, parsers and bitstream filters all stay, so no openable format changes. **Bit-identical output** to vcpkg on 4K 4444, 8K 4444 XQ, 4448x3096, 4K 422 HQ and 4K H.264 — 0 differing pixels, max channel delta 0.

  **FIVE PORTABILITY FAULTS, EVERY ONE FOUND BY RUNNING IT SOMEWHERE ELSE RATHER THAN BY READING IT.** They are worth keeping because each is a class, not an incident.

  **`.gitignore`'s `build-*/` was unanchored, so it matched at every depth and silently excluded `scripts/build-ffmpeg/`.** `git add -A scripts` reported success and added nothing; CI failed with "the term … is not recognized". **`git add` succeeding is not evidence a file was added — `git ls-files` is.** The four build-tree patterns are anchored to the repo root now.

  **`mingw32-make` is a NATIVE Windows make**, so it spawns each recipe through `cmd.exe` and inherits its ~32K command-line limit, while FFmpeg hands `makedef` every object file of a library as one argv. The build died with **`Object does not exist: libavcodec/h261_parser.`** — a filename truncated mid-word, which reads like a corrupt tree. It took three wrong diagnoses (bash, then object-list length, then encoders). **msys2's make spawns through the MSYS runtime and has no such limit.**

  **A dependency that resolves from your own toolchain directory is not a dependency you have satisfied.** `avutil-60.dll` imported `libwinpthread-1.dll` from winlibs' posix-threads runtime — *even though FFmpeg is built `--enable-w32threads`*, because that import belongs to the compiler runtime and not to FFmpeg. It passed here because gcc's bin was on PATH; the runner failed the renderer selftest with **exit -1073741511 (0xC0000139, STATUS_ENTRYPOINT_NOT_FOUND)**. Fixed with `--extra-ldflags=-static`, and **the durable fix is the assertion**: CI now runs `dumpbin /dependents` over **every** produced DLL and fails on anything outside the Windows system set. I had checked *avcodec* when the build was written and concluded "system DLLs only" — true of avcodec, false of avutil. **One DLL is not the set.**

  Two smaller ones: `vcvars64.bat` was hard-coded to VS **Community** (runners have Enterprise; discovered via `vswhere` now), and `bash -lc` sources `/etc/profile`, which **rebuilds PATH** and discards the pinned toolchain — configure then fails with "gcc is unable to create an executable file", which reads like a broken compiler.

  **And the pixel check accused a correct build**: a stride-unaware `LockBits` diff reported **399 differing pixels at max delta 171 on four unrelated files**. Identical figures across unrelated inputs is what gave it away — it was walking row padding between two locks. Tenth stale-instrument finding.

- **A GCC/mingw LGPL CANDIDATE IS PROVEN INSIDE TRACE AND IS BIT-IDENTICAL, BUT BtbN'S ARTIFACT IS NOT SHIPPABLE AS-IS (2026-08-13).** `ffmpeg-n8.1-latest-win64-lgpl-shared` has **identical sonames to vcpkg** (avcodec-62, avutil-60, swscale-9) and ships MSVC import libraries, so it is a drop-in DLL swap needing no Trace rebuild. Verified: renderer selftest passes, **pixel output is bit-identical on 4K 4444, 8K 4444 XQ and 4K 422 HQ — 0 differing pixels, max channel delta 0**, which covers 12-bit 4:4:4 with alpha through the accurate Step conversion path. Regression flat: 4K H.264 x3 100.0% `0 of 119`, scrub `-SnapRelease` `target 261 shown 261 delta 0`, aac audio mastering at 99.5% with `under 0 rep 1 skip 0`.

  **What disqualifies it is packaging, which requirement 1 names explicitly: 17.3 MB of DLLs becomes 104.3 MB** (avcodec 5.2x, swscale 12.7x) because BtbN statically links libaom, dav1d, vulkan/shaderc and much else that Trace never calls. That is ~87 MB on a portable ZIP and a large set of additional third-party licence notices for code that is never executed. **The gain is the toolchain, not the extra libraries** — so the correct artifact is a *minimal* mingw-GCC LGPL build carrying vcpkg's exact feature set. msys2 is already present under vcpkg's downloaded tools but **lacks mingw-gcc and nasm**, so that build needs toolchain installation and a CI step.

- **PLAYBACK HOLDS MEDIA TIME REAL-TIME AND DROPS PICTURE WHEN A SOURCE CANNOT KEEP UP — owner decision, 2026-08-13 (`35d976b`). This is a deliberate, bounded exception to "never skip a frame", and it is the fourth.** A source that cannot physically sustain its native rate keeps the *movie* on the clock rather than playing the whole thing slowly. **Exactness is untouched and stays mandatory** for paused inspection, frame stepping, click landing and scrub release. **1× forward playback and nothing else**: reverse and the shuttle carry their speed in a stride and return earlier, and **below 1× every frame is presented** — the user asked for slow motion, and dropping picture to hold a deliberately slowed clock would defeat the request (the same reasoning that makes 0.5× silent).

  **`realtimeDropSteps()` returns 1 unless the run is already behind**, so "engage only when required" is structural rather than a tuned threshold, and **`drop 0` across the validated asset set is the check**. The HUD reads `drop N (ticks M max R, media P%)`: `real time` is how much *picture* arrived, **`media` is whether the *movie* stayed on the clock**, and the second must read ~100% whenever the first reads below it.

  **THE CLOCK IS THE GATE E PRESENTATION TIMELINE, NOT THE WALL-CLOCK ACCUMULATOR.** The accumulator is capped at four periods so a stalled run resumes at rate rather than fast-forwarding through arrears — so `floor(accumulator/period)` **pins at 4** on a source running at half rate and would ask for nearly double speed. The timeline needed an anchor **frame** as well as an epoch (`presentAnchorFrame_`), or Play from the middle of a file maps media time onto the wrong frame.

  **THE FIRST CUT MADE IT WORSE — 14.4% of real time against 34.4% for not dropping at all — AND THE CAUSE IS A SECOND, SEPARATE PREMISE EXPIRY.** `kPlaybackForwardWalkLimit` was **4** on intra-only, so a present that skipped 3 frames *walked* them and decoded all 3 (`walk 3f`, `dec 215ms`). **It is a runaway as well as a cost**: slower playback falls further behind, which asks for a bigger jump. On intra-only a seek lands on the target for one decode, so **the limit is 1** — `+1` still walks as ordinary playback must, and any jump seeks. Long-GOP keeps 48, where a walked frame is ~2.6ms against a ~30ms seek and the trade genuinely inverts. Measured on the 8K plate: `media 97.4%`, `drop 67`, `walk 0f`, `hitch 0`, **frame 127 of a due 131 in 5.44s** against frame 60 of 133 before. `TRACE_RT_DROP=0` is the control.

- **PER-MODE THREADING (FRAME FOR PLAYBACK, SLICE FOR SCRUB) WAS BUILT, MEASURED AND TAKEN BACK OUT — 2026-08-13. The owner authorised it "if the measurements continue to support that", and they do not.** Do not rebuild it without reading this. It was a working implementation: a codec reopen at the mode boundary, safe under the scrub lease, with two-edge hysteresis on the latch.

  **DROPPING IS WHAT KILLED IT, and the reason generalises: the real-time drop makes playback ask for *jumps*, and on intra-only a jump IS random access — so the mode that would want frame threading stops existing exactly when the source is heavy enough to want it.** All four cells on the 8K plate, and three are catastrophic: **frame + seek 4.2%** (`dec 658ms`, a pipeline refill per frame) · **slice + walk 14.4%** (`dec 215ms`, decodes every dropped frame) · **frame + walk 16.7%** (`handler 253ms`) · **slice + seek 46.0%, `media 97.3%`** — and it is not close.

  **It was measured on both reported files and failed on both.** The 8K never earns frame threading, which is correct. The marginal 4448×3096 plate *does* earn it and is **worse for it — 92.3% presented with 7 drops against 98.4% and none** — because a frame-threaded `avcodec_open2` over a 4448×3096 frame costs a **280ms** stall, once, which the drop then has to catch up. Pinned to slice on the same build it reads 98.4% and `drop 0`. **`TRACE_INTRA_FRAME_THREADS=1` reproduces all of it.**

  Two things from building it that are worth keeping. **The hysteresis has to be two-edged and the second edge is not obvious**: losing frame threading on the *first* drop made a marginal file oscillate (`sw 6` in four seconds, `max 331ms`), because a source that drops one frame in sixty is a sequential workload with an occasional jump. And **the starting state must be the safe one** — starting frame-threaded and switching out cost two reopens and `max 892.9ms` on a 5s run, while starting in slice let the heavy source discover it could not keep up for `sw 0`.

- **GATE E's deadline scheduler QUANTISED PLAYBACK TO fps/N ON ANY FILE THAT MISSED ITS BUDGET — FIXED 2026-08-13 (`ee6d525`).** `armNextPresent()`'s rephase branch arms for the next grid slot *strictly after now*. That is right for a **transient** overrun — one long frame costs one slot and the run resumes at rate rather than fast-forwarding through arrears — and wrong for a **sustained** one, because then every arm inserts the remainder of a slot as idle. With `handler 88ms` against a `41.71ms` period (2.11 slots) it armed for slot 3 and played at `23.976/3 = 8.0fps` while the pipeline could supply 11.0. The signature is **`outside 32ms` of a 121ms period with `rephase` firing on every frame**.

  The fix is per frame and needs no new state: **`lastHandlerMs_` is valid inside `armNextPresent()` because the `recordHandler` scope guard is declared AFTER the `armNext` guard and so runs BEFORE it.** A handler that fit its period and is still late is jitter and keeps the grid; one that did not fit is a saturated pipeline with nothing to wait for. Epoch and slot are untouched, so the timeline does not move and the frame index is still the accumulator's or the audio clock's — **no frame is skipped, and this is not an exception to that rule.**

  **It is inert on everything that meets budget, and `rephase 0` across the validated set is why** — the branch never executes there. 4K H.264 ×3 100.0% `0 of 119`, 4444 ×2 99.8% `0 of 260`, 4K 60fps ×2 100.0% `0 of 161` against a 16.67ms budget, both lifecycle legs, case for case with the control. **The cadence gets EVENER as well as faster** on the file that was failing (p50/p99 122.8/131.5 → 91.0/100.5), so there was no steady-cadence argument for the behaviour it replaces. `TRACE_SCHED_FREERUN_LATE=0` is the control **in the same binary**.

  **This is what a GATE E premise looked like when it expired**: the scheduler was validated in Aug 2026 on files that meet budget, where sleeping to the next deadline is exactly right, and no file in the validated set overran until a 7680x4320 plate arrived.

- **INTRA-ONLY SLICE-ONLY THREADING IS A LIVE TRADE, NOT A STALE DECISION — re-measured 2026-08-13 (`ed686a1`), premise INTACT.** The July 2026 rule forcing ProRes/DNxHD/MJPEG to `FF_THREAD_SLICE` was re-examined on the assumption that `f77d472` (async scrub worker) had expired it. **It has not.** `TRACE_INTRA_FRAME_THREADS=1` is now the symmetric control that `TRACE_LONGGOP_SLICE_THREADS` never had, off by default, and both halves are large: forward playback on a 7680x4320 4444 XQ plate **33.8 → 58.0%** of real time with the handler **89.7 → 43.3ms**, against 4K 4444 `scrub -SnapRelease` `dec 15.9 → 155.6ms`, `release 42.8 → **398ms**`, `ui gap max 26 → **241ms**`, and `-Reversals` `hitch 2 → 7`. **The landing stays exact on both legs** (`target 261 shown 261 delta 0`), so this is responsiveness, not correctness. The worker absorbs the *drag*; **the release landing is still synchronous and that is where the pipeline refill is paid.**

  **A REFERENCE-DECODER BENCHMARK ANSWERS THIS QUESTION WRONG AND ANSWERS IT CONFIDENTLY.** `ffmpeg -f null` on this box makes slice-only look *faster* on every ProRes file in the set — 4096x2304 78.2 vs 75.0 fps, 4448x3096 59.1 vs 50.8, 7680x4320 XQ 19.9 vs 17.5 — which would have closed the question as refuted before the knob was built. `-f null` decodes and discards, so there is nothing for a frame-threaded decoder to overlap **with**, and frame threading only adds its per-thread state copies; Trace's tick decodes and then converts, uploads and paints on the same thread. **Eighth stale-instrument finding and the first that is out of process. A benchmark that removes the work your program does around the thing being measured is measuring a different program.**

  **The way out is not a resolution- or codec-conditional default** — a big plate needs good scrub as much as good playback. `thread_type` is a property of **what the decoder is being asked to do**: playback wants frame threading, random access wants slice. Switching it needs `avcodec_close` + `avcodec_open2` at the transition, and the reopen must not land inside a scrub lease. That is its own piece of work, worth ~1.5x on large intra-only playback at no cost to scrub.

- **LARGE ProRes 4444 IS PIXEL-LINEAR AND HAS NO PATHOLOGY — measured 2026-08-13, physical panel.** An owner report of very slow playback on `13_4448x3096_ProRes_4444` and `12_8K_ProRes4444` resolved into two different answers. **The 4448x3096 file does NOT reproduce as slow**: 98.4% of real time at the §4 opening size and 98.3% maximized at `win 5120x1369`, `hitch 0` both, `handler>budget 1 of 106`. **The 7680x4320 file is ProRes 4444 XQ at 5,739 Mbps** (not 8192 wide, not plain 4444) and **cannot be played at real time by anything on this machine** — `ffmpeg -f null`, decode alone with every other stage deleted, reaches **20.5 fps = 85% of real time**. A player that looks smooth on it is dropping frames or running a proxy; that is a contract difference, not a faster decoder.

  Per-frame terms are **linear in pixels with nothing anomalous** — dec/Mpx 1.56 / 1.44 / 1.81, sws/Mpx 0.55 / 0.53 / 0.53, upload/Mpx 0.34 / 0.38 / 0.34 across the 4K control, the 4448 and the 8K. **Do not go looking for a bug in the conversion or the upload.** I/O is refuted by a wide margin: `io play … seq 100.0% | seek 0 | lat 2.696/6.6ms | **44,526 Mbps** | stall 0` against a 5,739 Mbps file on a Samsung 990 PRO. And **GATE C's planar conclusion gets STRONGER at 8K rather than weaker** — `TRACE_PLANAR_UPLOAD=0` takes `sws 17.6 → 57.9ms` to save 4ms of upload, giving 29.2% against the planar default's 34.4%, with `TRACE_RENDERER=cpu` at 32.4%. The byte arithmetic that predicted otherwise was right about bytes and wrong about which term dominates: **the planar win is a memcpy replacing a colour conversion, and the conversion scales with pixels exactly as the copy does.**

- **The integer tick beat is FIXED — GATE E step 1, Aug 2026, plan §24.13.** The playback tick was a fixed integer-millisecond `QTimer` at `floor(1000/fps)`, so presents landed on a 41ms grid and every interval between two of them was 41 or 82ms and never 41.667. It is now **re-armed per frame against an absolute deadline** built from the source's exact rational: `deadline(slot) = epoch + slot × period`, in nanoseconds, never rounded. Only the delay handed to `QTimer` is rounded, and because the next delay comes from the next *absolute* deadline rather than from this one, that rounding cannot accumulate — the arms alternate 41/42 and average the true period.

  **`presentSlot_` is a grid slot, not a frame count.** It advances on every wake whether or not a frame was presented, so the heartbeat stays regular and "which frame" stays entirely the audio clock's question. This is the §9 composition rule with the phase half done in software: **the accumulator gate was removed for video, not made conditional** — `cd79d49` is on record for what happens when two clocks each decide half of "when to present".

  Results, `win 1280x815`, `TRACE_NO_AUDIO=1` for the controls. 1080p: the 1.5–2.5x bucket **5/4 → 0/0**, long-gap spacing **57/58/59 → none**, p50 **41.0 → 41.9** (the true 41.71), max **82.9 → 43.9**, drift **−13 → 0ms**. 4444 on the planar d3d11 path: **0 doubled frames, 0 handlers over budget, max gap 45.9ms**, 99.8% real time. Audio-mastered files all improved — 1080p 99.1 → **99.6%**, 4K H.264 98.3 → **99.1%**, 422 HQ 98.4 → **99.2%**, with `rep` **4–5 → 1** and `skip 0`.

  **`TRACE_DEADLINE_SCHED=0` restores the old scheduler in the same binary**, which is the negative control §24.9 required and is better than a control build — the two runs differ in one branch rather than in a compile. It still shows the fault.

  **A metric broke in a way worth remembering.** The first 4444 run read `jitter 34.00` and looked catastrophic. The timer is re-armed at the *end* of the handler, so the armed interval excludes the handler's own 33ms while the wake-to-wake delta includes it — `tickDelta − armedInterval` had silently become a measure of decode cost. Jitter is measured against the **frame period** now (what it always meant; before GATE E the armed interval *was* the period) and the same run reads **0.65/2.49**. A derived metric whose inputs changed meaning reads as a terrible result, not as a broken metric.
- **The display is 239.999Hz, and only `QueryDisplayConfig` can tell you that** (Aug 2026, `scripts/measure/refresh.ps1`). `EnumDisplaySettings` reports an integer "240" and cannot separate 240.000 from 239.76 (=240000/1001, a very common "240Hz" mode) — which is why §22.8's recorded "239Hz" was not evidence either way. At 239.999 a 24.000fps frame is **exactly 10 refreshes** (one slip per 24,000 frames), so the display imposes essentially nothing on the test set. **23.976 content is the interesting row: 10.0100 refreshes, a slip every ~100 frames.** That is the display's beat, not Trace's, and no player removes it.
- **`DwmGetCompositionTimingInfo` does not work on this machine** (Aug 2026): `0x88980090` for NULL, desktop and shell HWNDs, at three `cbSize` values, from an interactive process on `WinSta0\Default` with `DwmIsCompositionEnabled` true. It is deprecated and entitled to refuse. Consequence: **there is no renderer-independent vblank phase source**, so any grid-snapping work is d3d11-only via `IDXGISwapChain::GetFrameStatistics`. E1 needs no phase source at all, which is why the cadence fix still reaches the default CPU path. Note the first probe returned the *same* HRESULT because its struct was 184 bytes instead of 320 — rule out marshalling before reading an HRESULT as an answer.
- **~~The playback stutter is the integer tick beat, it is on EVERY file, and only GATE E fixes it~~** — the characterisation stands and is the baseline the fix was measured against; the fault itself is now fixed (entry above). (Aug 2026, plan §23). The owner reported ProRes 4444 not locked to real time. Measured with a cadence *distribution* rather than the presented rate — which reads 98–99% under two unrelated faults and therefore cannot tell them apart, and that is exactly how 4444 measures 99.6% and still stutters.

  At `fps=24.000000` (confirmed via `TRACE_OPEN_LOG`, not assumed) the tick is `floor(1000/24)=41ms` against a 41.667ms frame, so the accumulator falls 0.667ms short per frame and every `41.667/0.667 = 62.5` frames needs two ticks. **Measured median spacing between long frames: 61–62, on all six runs.** Four to five doubled frames per 10s, `max ≈ 2 × p50`, and **nothing at all** in the 1.1–1.5x or >2.5x buckets — a tight spike plus clean doublings, which is a beat and not cost.

  **The 1080p control is the proof.** Its worst handler is **3.8ms** against a 41.67ms budget — ten times the headroom — and it shows the *same* four doubled frames at the *same* 62-frame spacing. Do not attribute this to decode cost on any file.

  **Audio does not remove it**, which is the easy wrong assumption: the audio clock picks *which* frame, but a frame can only be presented on a tick, so a held frame still doubles the interval. `rep`/`skip` read 0 while it happens.

  **The control MUST use `TRACE_NO_AUDIO=1`.** 4444 has no audio track while 422 HQ and the 1080p clip do, so as shipped they run on different schedulers; comparing them directly would "prove" 4444 is uniquely bad when the only difference is which clock is driving.
- **GATE C already removed 4444's per-frame-overrun component; the beat is all that is left** (Aug 2026, plan §23.4): on `cpu` and on d3d11-BGRA, 4444 read tick jitter max **11–14ms** and one handler *over* the 41.67ms budget at 55.6ms; on the planar path it reads **2–3ms** jitter, **zero** over budget, worst handler 37.6ms. A 25ms handler delays the timer, a 10ms one does not. Since `cpu` is still the default, the owner's stutter report likely includes a component the planar path no longer has.
- **Presentation is NOT frame-rate locked, and `Present(0,0)` is not display-synchronized** (Aug 2026, audited at GATE B — plan §20.5). Four facts, kept together because each is separately easy to misremember: (a) the **exact rational is stored** (`VideoMetadata::fpsNum`/`fpsDen`); (b) **nothing reads it** — every consumer goes through `FrameSource::fps()`, a double, and the tick is `floor(1000.0/fps)`, an integer-millisecond QTimer (41ms at 23.976); (c) `Present(0, 0)` uses **sync interval 0** — not vsync-throttled, not phase-aligned; DWM composites at refresh so at most one present is seen per refresh, but nothing in Trace knows the refresh phase; (d) **cadence and refresh synchronization are GATE E**, not GATE B or C. The accumulator does not drift — `frameDurationMs` is a double fed by `nsecsElapsed()` and carries its residue forward, so the tick *bounds* the rate rather than setting it. **Do not describe the rational as frame-rate lock**: its value is as an unrounded reference for measuring cadence, not as a rate correction.
- **The source frame rate is kept as a rational** (Aug 2026, `7b924be`): `metadata_.fps = av_q2d(fr)` discarded the `AVRational` on the spot, so 24000/1001 became the nearest double and the tick interval, timecode and seek arithmetic all worked from an approximation. `VideoMetadata` carries `fpsNum`/`fpsDen` alongside it. Nothing reads the pair yet — this is a prerequisite for GATE E, where a rate that is already rounded cannot be the reference for late-present or jitter. `int`/`int`, not `AVRational`: the header is reached from `MainWindow.h` and must compile with `TRACE_WITH_FFMPEG` undefined, the same rule that keeps `AVPixelFormat` out of `VideoFrame.h`.
- **The slider handle belongs to the user while the user is holding it** (Aug 2026, `f77d472`): `syncTransportBar` wrote the *decoded* frame back into the slider on every HUD refresh — several times a second during a drag — so the handle was yanked out from under the pointer and the next mouse move dragged it back. **That is the "slider not keeping up with the pull" report, and it was never event-loop starvation**: the handle was being moved somewhere else on purpose. It also corrupted the landing, because `sliderReleased` lands on `timelineSlider_->value()`: a fast 1080p reversal set landed on frame 30 instead of the 3 the user pointed at. Guarded on `isSliderDown()`.
- **Presentation goes through `VideoRenderer`** (Aug 2026, `5765c19`, `src/render/`): `CpuImageRenderer` is the only backend and holds the existing paint verbatim. **The renderer owns the whole paint, including the no-frame placeholder** — two painters cannot share one paint event, and a D3D11 backend will not use `QPainter` at all. There is deliberately no `QPainter` in the interface; `paint(QWidget*)` lets the CPU backend make its own and a GPU one ignore the host. `ViewerWidget` keeps only what is the host's business (when a repaint was asked for, how long it took to arrive) and folds `RenderStats` into `ViewerPerfStats` so the HUD reads one struct. `TRACE_RENDERER` selects the backend, defaults to `cpu`, and warns on stderr before falling back; the HUD `renderer` field names what is actually presenting, because **a GPU path that quietly never engages while the app looks fine is the failure mode to design against**.

  **A second backend exists now** (Aug 2026, `8a7cdb3`, GATE B): `D3D11VideoRenderer`, opt-in via `TRACE_RENDERER=d3d11`. Two things about the boundary changed and both are load-bearing. `usesNativeSurface()` is a **widget-level** contract asked of the renderer — the host has to realise a native window and stop erasing the widget *before* `initialize()` runs, because that call is what realises the HWND the backend attaches to; `ViewerWidget::adoptRenderer` is the one place that applies it, so the ordering is a property rather than a convention. And **fallback moved to the host**: `createRenderer()` can only decline a backend it *knows* cannot run, while a GPU backend fails for reasons that only exist once there is a device and a window, so `ViewerWidget` adopts `createCpuRenderer()` on failure and says which one is presenting. The D3D11 surface is a **child HWND**, not the widget's own — see plan §17.2, including the correction that the `WA_PaintOnScreen` alternative also works and the first reason recorded against it was wrong.
- **The decoder must be drained at end of stream** (Aug 2026, `e76eabb`): `av_read_frame` returning EOF is not the same as having no frames left — frame-threaded codecs hold up to `thread_count` frames in flight. Without the null flush packet the tail of every long-GOP file was never displayed (15 frames of a 96-frame clip on a 32-core box), and `frameAt` returned *true* carrying the previous image, so the viewer repeated the last frame while the counter ran on and each request past it paid a pointless seek. `frameAt` now returns false when the codec is exhausted and the frame was not produced — never substitute a stale image for a missing one. Drain state resets on open and on the flush after a seek.
- **Forward-fill queue was removed** (July 2026): it decoded up to 4 frames per timer tick in bursts and caused rhythmic stutter on 4K ProRes. Don't re-add synchronous read-ahead.
- **Every seek is frame-exact, Scrub included** (Aug 2026, supersedes the mid-scrub-drag exception from July 2026 and the keyframe-label gap from 7a3fa95): after any seek, the first decoded frame's index is resolved from its PTS (`seekResolvePending`) and decode continues forward to the true target. Files without PTS fall back to label-as-target. Cost: seeks on long-GOP H.264 decode up to a GOP of frames.

  The removed exception is worth remembering as a failure mode. Scrub used to skip PTS resolution and *label the landed keyframe as the requested frame* — instant, and wrong by up to a full GOP. Measured on the 1080p validation clip: dragging across frames 49→55 displayed **keyframe 30 for all seven**, while the HUD read `scrub exact | delta 0`. Releasing at 55 then walked 25 frames and showed a completely different shot. Two things made this survive: the tradeoff was recorded as deliberate, and **the telemetry asserted its own correctness** rather than measuring it. `shown`/`delta` are computed now. A review tool cannot display one frame and name it another.
- **Dragging the slider is a shuttle, not a sample** (Aug 2026): a *click* jumps to a point (press+release, release forces the exact target through Step); a *drag* walks the decoder through every frame between the last shown one and the pointer and puts each on screen. This inverts what gets paid for — seeking was the expensive half (keyframe landing plus GOP walk), stepping forward is ~1ms at 1080p — so `RequestMode::Scrub` no longer forces a seek and genuine jumps go through the ordinary backward/large-gap conditions. Measured on the 1080p clip: a slow drag went from **2 seeks and one new picture per GOP** to **every frame painted, `walk 0f`, `delta 0` and true**.

  **A forward drag never jumps** (Aug 2026, supersedes the first cut of this entry). Snapping to the pointer when the gap grew was tried and rejected by the owner as "really harsh" — it skipped runs of frames, which reads as tearing through the clip rather than shuttling it. The picture is allowed to trail the pointer instead, and only a *click* jumps.

  How far a slice advances is **eased**, not fixed: it covers a constant fraction (`kScrubEase`, 0.5) of the remaining distance, giving an exponential approach that moves fast when far behind and settles gently onto the target rather than arriving with a jolt. The time budget and the easing swap over as the limiting term — budget-bound while far away, ease-bound as it converges — so the motion accelerates and decelerates without either being scheduled.

  **Catch-up slices re-arm at zero interval, not the coalescing interval** (Aug 2026). This is the setting that decides how tightly the picture tracks the pointer, and getting it wrong reads as looseness rather than as slowness. `kScrubCoalesceMs` (12ms) exists to stop a burst of slider events costing one decode each; leaving it in the catch-up path capped the shuttle at one slice per 12ms *plus* the slice's own ~8ms of work — about 45 slices a second — and a quick drag outran it and trailed further and further behind ("lagging too far behind", "feels really loose"). Zero-interval still goes through the event loop, so pointer moves and repaints are serviced between slices. Steady-state lag under a constant drag is roughly (frames the pointer moves per slice) / `kScrubEase`, so both terms matter: raising the fraction and removing the throttle together took a 6x-speed drag from visibly trailing to **zero lag**.

  Measured envelope at 1080p, continuous sweeps across the whole clip: at **6x playback** (216 frames in 1.5s) peak lag is **0** with 221/221 frames painted and 2 seeks; at **20x** (216 frames in 0.45s) peak lag is 70 frames, which then eases back to 0 in ~400ms while painting every intermediate frame. Throughput ceiling is ~325 frames/sec, about 13.5x. Nothing is ever skipped at any speed — overrun shows up as lag, never as a jump.

  **Shuttle presentation pacing: written, measured, DEFAULT OFF** (Aug 2026). A slice that lands a run of cache hits paints a dozen frames inside 8ms -- far faster than the panel samples them, so most are overwritten before any refresh sees them -- and then stalls on the next miss. The eye gets a couple of frames, a freeze, a couple more, which is why *backward* drags felt jumpy while forward (uniform ~1.8ms/frame) did not. Pacing to one frame per refresh does even the motion out and every painted frame is then genuinely shown. **It was still rejected on feel**: the re-arm round trip caps the paced rate near 140fps rather than the panel's 240, which costs forward roughly 20 frames of lag at a 6x drag, and forward smoothness is the thing that was signed off. `TRACE_SCRUB_PACE` keeps it available (0 off = default, 1.0 one frame per refresh). Do not enable it by default again without re-testing **fast forward drag** specifically -- that is the case it regresses, and the backward case it fixes is the one that is already known to need a cache fix instead.

  **`viewer_->repaint()`, not `update()`** — update() coalesces, so a walk loop would decode every frame and display only the last, which is the bug being fixed.

  Measured on the 1080p clip, a fast drag traversing ~79 frames: **82 paints, 2 seeks, `lag 0f`, `delta 0`, full-res**. Before: 12 seeks, jumping ~8 frames at a time, keyframes only. 4K ProRes 422 HQ shuttles at 11.9ms/frame on the same test and still reaches `lag 0f` — heavy media trails further mid-drag but is not special-cased.

  `scrubWalkPerFrameMs_` (EMA of the walk loop's own timing) is retained as the HUD's shuttle-rate figure and is the first number to check if a drag feels slow. It is measured rather than taken from `VideoPerfStats` averages, which pool seek-walk decodes and read ~4x the true sequential cost (`dec 0.07` last vs `5.02` avg).

  **Backward drags shuttle too** (Aug 2026) — the same walk with the sign flipped. It is affordable because a backward step that misses the reverse cache costs a seek plus a GOP walk, and *that walk fills the cache on its way through*, so one miss is followed by a run of hits covering the rest of the GOP. The time budget absorbs the miss (one frame that slice, then re-arm) instead of letting it stall the drag. Two things had to change for it to work: the walk became direction-aware, and **the presented frame is now cached when it is full-res** — previously no Scrub frame entered the cache at all because previews used to be half-res everywhere, but above 1920px is the only place they still are.

  Measured at 1080p, continuous backward sweeps: at **6x** peak lag **0** with 221/221 frames painted and **91.4% cache hits** at 1.50ms/frame — marginally *faster* than forward, since most frames are hits rather than decodes. At **20x**, 417/417 painted at 92.1% hits. **4K H.264 is the weak case**: 28.9ms/frame, 57.7% hits, and it trails badly (59 frames behind 400ms after the pointer stops). Three things stack there — cache capacity is 6 rather than 24 (footprint-derived), previews are half-res so the presented frame is still not cached, and a miss on long-GOP costs a full seek-and-walk. It never skips a frame; it is just slow. 4K ProRes is fine by comparison because every frame is a keyframe, so a miss costs no GOP walk.
- **The frame cache is budgeted in bytes, and previews convert to the displayed size** (Aug 2026, `b5a56af`, supersedes the entry-count capacity below and the flat half-res rule). Both were the same mistake: pricing a scrub preview as though it were a full-resolution frame.

  Capacity was `192MB / (w*h*4)` — six entries at 4K — but a preview costs a quarter of that or less, so the cache sat at 47MB of its 192MB budget while a backward drag missed on nearly every frame. Six entries cannot serve a walk back through a thirty-frame GOP however good the hit logic is. Eviction is on summed `sizeInBytes()` now; the same budget holds 24 at half res and ~150 at display res. The reported `cap` in the HUD is derived from the size currently stored, so it moves as a drag fills the cache.

  Previews convert straight to the size the viewer will draw them at, capped at half resolution and never upscaled. Converting 4096x2304 → 2048x1152 to show it in a 640x360 widget does four times the pixel work that reaches the screen and then hands the surplus to Qt's raster bilinear, which is the weaker resampler — so this is the expensive half of the frame getting **cheaper and sharper at once**, and the viewer now draws previews 1:1. Measured on 4K ProRes 422 HQ: `sws 7.08 → 1.87ms`, total `11.18 → 7.32ms`. The cache is cleared when the size changes (`setScrubPreviewSize`), because entries carry the size in force when they were made; mixed sizes in one drag read as the picture breaking up. `TRACE_PREVIEW_DISPLAY_SIZE=0` restores the old rule.

  **Seek-walk cache fills follow the request mode.** A Step landing keeps the 18ms budget — one frame is wanted and every speculative conversion is delay in front of it. A Scrub seek happens mid-drag, where the frames walked past are exactly the ones the drag is about to ask for in reverse, so declining them means paying the whole seek and GOP walk again for each; those get 60ms (`TRACE_SCRUB_FILL_MS`). Scrub fills are stored at preview resolution and **tagged**, and stepping refuses tagged entries — the old code paid for a full-res fill and then declined it anyway.

  **This does nothing for ProRes backward, for a structural reason worth remembering**: every frame is a keyframe, so a backward seek lands directly on the target with no frames walked en route, and there is nothing to cache. ProRes backward measured 0% hits before and after. Its improvement came entirely from the cheaper conversion. Don't "fix" ProRes hit rates by enlarging the cache.
- **Reverse cache is sized by cost, not by frame count** (Aug 2026, **superseded by the byte budget above** — kept for the reasoning): capacity used to be set at open from frame footprint (~192MB budget → measured **6 frames at 4K** (31.6MB each), **24 at 1080p** (7.9MB each)), and the seek-walk fill window is whatever fits an ~18ms conversion budget using measured `avgConvertMs`. A fixed count is wrong in both directions: at 0.7ms/frame (1080p H.264) caching a lot is nearly free and saves whole GOP re-walks on backward stepping, while at 14ms/frame (4K) each entry is latency the user feels on the landing frame. The Aug 2026 fixed window of 4 fixed scrub landing but made repeated backward stepping seek ~3x more often. Env `TRACE_SEEK_CACHE_WINDOW` still forces a count; HUD `walk Nf cache Ncv/Nms` shows the cost.
- **Alpha planes are stripped before conversion** (Aug 2026): ProRes 4444 decodes to `yuva444p12le`, and the viewer draws `QImage::Format_RGB32`, which ignores alpha — so scaling that full-res 12-bit plane was pure waste. `alphaStrippedFormat()` re-describes planar YUVA buffers as their alpha-less equivalent (planes 0–2 are byte-identical; plane 3 just never gets read). Only applied to PLANAR formats: packed formats like `rgba` interleave alpha per pixel, so stripping would corrupt the layout. `TRACE_KEEP_ALPHA=1` restores the old behavior; HUD shows `(a-skip)` when active.
- **Audio is the playback master clock, and it is the one exception to "never skip frames"** (Aug 2026): the sound card's rate is the only rate in the system that cannot be negotiated with, so during 1x forward playback the target frame comes from the device clock rather than the wall-clock accumulator. This also lifts the 23.81fps tick ceiling. Corrections are bounded: hold the current frame when sound has not reached the next one (never re-request the same index in Playback mode — that advances the decoder and is exactly the frame-order bounce the linear invariant prevents), advance at most 3 frames to catch up. **Stepping and scrubbing remain exact always** — this only affects continuous playback with sound. With no audio track, or any time audio is not driving, the old wall-clock path runs unchanged.
- **Playback jumps within 4 frames walk instead of seeking** (Aug 2026): needed for audio catch-up. A seek costs a keyframe landing plus a GOP walk (~60ms on long-GOP H.264) to avoid decoding two frames forward — strictly the wrong trade. Scoped to `RequestMode::Playback`; Step and Scrub seek behavior is untouched.
- **Audio is 1x forward only** (Aug 2026): J-K-L off-speeds, reverse, scrub and step are deliberately silent. Resampled and reversed audio is separate work, and half-working sound is worse than none in a review tool. One guard in the tick catches every way playback stops being 1x forward; every `playTimer_.stop()` is paired with `stopAudio()`, because with no more ticks that guard cannot run.
- **A custom pull-mode QIODevice MUST override `bytesAvailable()`** (Aug 2026, first real audio bug): `QAudioSink` asks how much is readable before it reads, and parks in `IdleState` if the answer is zero, waiting on a `readyRead()` that a hand-written device never emits. `QIODevice::bytesAvailable()` defaults to counting only its own internal read buffer — which this design does not use — so it returned 0 forever. The sink started, pulled nothing, `processedUSecs()` stayed 0, the audio clock sat pinned at its start value, and **the picture froze solid** while the transport said Play. Diagnosed from the HUD's raw sink fields (`proc 0ms sinkbuf 96000 free 96000 state 3`); `state 3` is `IdleState`. Those raw fields stay in the HUD — the derived `sync` figure alone could not say which term was wrong.
- **The audio clock runs on wall time and is disciplined by audio, not sampled from it** (Aug 2026): `processedUSecs()` counts bytes handed to the device, so it advances in whatever chunk the sink last pulled — a staircase, not a ramp. Reading media time straight off it made the playhead oscillate about a frame either side of true, and the tick paid for that with roughly 1.2 held and 1.2 skipped frames *per second* on a file with 40x decode headroom. A first-order loop (wall-clock projection, slew 0.05 toward the raw audio value, snap above 0.25s for real events like startup fill or a stall) plus a monotonic clamp fixed most of it. **Don't replace this with a direct read of `processedUSecs()`** — that is what it is there to filter.
- **The playback tick interval is `floor(1000/fps)`, not `round`** (Aug 2026): `round` puts the tick at 42ms for a 41.71ms frame (23.976fps), systematically *slower* than the frame rate, so presentation can never keep up. The tick must be a bound on frame duration and let the clock choose which opportunities to use. Measured honestly: **floor alone changed almost nothing** (93.5% vs 94.4% — the churn had a different cause, above); it is kept because it is provably necessary, not because it moved the number. This is *not* the short-poll scheduler in the comparison table at the timer setup — that stays rejected.
- **Audio-master sync, measured Aug 2026** (each row adds to the one above):
  | change | 4K ProRes 422 HQ (168f) | 1080p H.264 9x16 23.976fps |
  |---|---|---|
  | freeze fix only | 152 frames, 87.4% real time, rep 19 skip 16 | 310f/13.7s, 94.4%, rep 14 skip 13 |
  | + smoothed latency EMA | 158 frames, 91.4%, rep 12 skip 10 | — |
  | + floor tick + disciplined clock | **164 frames, 95.0%, rep 6 skip 4** | **314f/13.7s, 95.3%, rep 11 skip 10** |

  Video tracks the clock correctly in all cases (frame index matches `clk x fps`); the residual is hold/skip churn of roughly 1/sec, which is clock jump, not decode. Presented-fps reads below real time because skipped frames are not presented — it is not a decode deficit. **The residual was resolved Aug 2026 — see the single-scheduler entry below; it was not clock jump.**
- **Under the audio master clock, the wall-clock accumulator must not also gate presentation** (Aug 2026): this was the actual cause of the residual hold/skip churn, and it was a scheduling bug, not a filtering one. Two clocks were answering different halves of one question — `playbackAccumulatorMs_` decided *when* to present, the audio clock decided *which frame*. The tick is `floor(1000/fps)` = 41ms against a 41.667ms frame, so roughly every 62nd tick the accumulator came up short and returned early without presenting; by the next tick the audio clock had advanced two frames, and one was dropped. Holds and skips therefore arrived in matched pairs at the beat frequency of the two clocks. **With audio driving, the audio clock is now the only scheduler** (the accumulator gate is bypassed, and still maintained so handover is clean if audio stops). Measured on the 1080p H.264 validation clip: **skips 7 → 0, frames presented 233 → 240 of 240, 95.1% → 99.1% of real time, drift −502ms → −86ms**. The no-audio wall-clock control on the same clip is 98.7%, so audio-mastered playback is now at (marginally above) the no-audio path. Diagnostic order matters here: the latency EMA, the slew gain, the snap threshold and a startup-priming gate were each measured first and each changed nothing — don't re-try them as fixes.
- **The playback accumulator must be fed nanoseconds, not `QElapsedTimer::restart()`** (Aug 2026): `restart()` returns whole milliseconds and discards the remainder, so the wall-clock accumulator lost an average of 0.5ms per tick — a systematic rate deficit proportional to tick frequency, and therefore *worse at higher frame rates*. Predicted loss is `ticks/sec x 0.5ms`; measured before/after, with the audio-driven files as controls:
  | file | before | predicted | after |
  |---|---|---|---|
  | 4K H.264 **60fps**, no audio | 96.4% | ~96.9% loss-adjusted | **99.8%** (drift −102 → −6ms) |
  | 1080p 24fps, `TRACE_NO_AUDIO` | 98.7% | ~98.8% | **100.0%** (drift −128 → −5ms) |
  | 4K ProRes 4444, no audio | 98.3% | ~98.8% | **99.4%** (drift −188 → −66ms) |
  | 1080p 24fps **with audio** (control) | 99.1% | unchanged | **99.1%** |
  | 4K H.264 with audio (control) | 98.3% | unchanged | **98.3%** |
  | 4K ProRes 422 HQ with audio (control) | 98.4% | unchanged | **98.4%** |

  Audio-mastered playback was never affected because the audio clock supplies position there. The fix reads `nsecsElapsed()` then `start()`, losing only the few hundred nanoseconds between the two calls (~0.0006%) and needing no extra state — every existing `start()`/`invalidate()` site keeps working because the timer remains its own reference. **Note the telemetry clocks are still integer-millisecond** (`schedulerTickClock_.restart()`, the period/jitter metrics), which is why `jitter` reads as whole numbers; that is measurement precision, not playback timing, and belongs with the Phase 1C cadence metrics.
- **Display refresh rate is NOT the remaining smoothness gap** (Aug 2026, measured on the physical panel with Parsec off): the same build, same clip and same window size was run at three rates on a Samsung Odyssey G95SC at 5120x1440 — **59Hz (2.4975 refreshes per 24fps frame), 119.98Hz (4.9992, effectively exact 5), and 240Hz (exactly 10)**. Trace's counters were flat across all three (99.1% / 99.1% / 98.7%, rep 4/4/5, skip 0/0/1) — expected, since nothing in the current path is display-synchronised, so the counters *cannot* move. The real test was subjective, and the verdict was **"about the same" at 120Hz and "slightly smoother, honestly hard to tell" at 240Hz**. Two consecutive runs at a *single* rate span rep 4–6 / skip 0–1, so the spread within one rate equals the spread across all three. **Do not promote the DXGI presentation-timing work on cadence grounds** — and note the logical point the measurement confirms: the 2:3 cadence at 59.94Hz is imposed by the display on *every* player equally, QuickTime included, so it cannot explain a Trace-vs-QuickTime difference. Whatever the gap is, it is something Trace does differently at the same refresh rate. Remaining suspects, in order: held frames (`rep` 4–5 per 10s, from the 41ms tick against a 41.667ms frame) and fit-to-window scaling quality (the validation window shows 1920x1080 at 666x375, a 2.88x downscale, past where Qt's raster bilinear holds up).
- **The audio device buffer is set explicitly, not inherited from the driver** (Aug 2026): the default is not stable across machines or Qt versions — CI (Qt 6.7.2) reported 192000 bytes, the local build (Qt 6.10.2) 96000, which on this device's **float stereo** format (8 bytes/frame, 384000 bytes/sec) is 500ms vs 250ms. Note that byte→duration conversion: assuming 16-bit stereo doubles every figure, which is how a 250ms buffer got read as 500ms once already. Buffer size is a real second-order term — with single-scheduler timing in place, measured 500ms: 95.3% / 14 holds / sync max 380ms; 250ms: 97.6% / 8 holds / 130ms; **100ms: 99.1% / 4 holds / 62ms**. 100ms reaches the no-audio control, so shrinking further only buys dropout risk. Ring capacity is now derived as ≥2x the device buffer, which keeps startup silence padding at 0 (it accrues only at end of stream, after the audio track runs out).
- **`clockSeconds()` was a control loop that reading it would step** (Aug 2026): it was declared `const`, mutated the loop state, and was called both by the playback tick and by `refreshHud()` → `stats()` — so the effective gain was double the tuned value and telemetry was moving the playhead. Split into `advanceClock()` (mutating, called by the tick alone) and `peekClock()`/`clockReady()` (pure observers). The HUD's `clk-upd last/max` counter measures updates *between consecutive tick entries*, so it catches a stray step from anywhere, and must read `1/1` while audio drives. HUD-visible vs HUD-hidden runs measure equivalent (99.1% vs 98.9%, zero skips both).
- **`TRACE_NO_AUDIO=1` is the control test to reach for first** (Aug 2026): it makes `AudioOutput::open()` behave exactly as a picture-only file, so video runs the wall-clock path with nothing else changed. It is what proved the judder was the audio clock rather than decode (240/240 frames and zero corrections with audio off, against 233/240 and 16 corrections with it on) before a line of the clock was touched. Companion knobs: `TRACE_AUDIO_BUFFER_MS`, `TRACE_AUDIO_SLEW`, `TRACE_AUDIO_FIXED_LATENCY`.
- **The audio clock's latency term must be smoothed** (Aug 2026): the device drains in chunks, so `bufferSize - bytesFree` sampled at an arbitrary instant is a sawtooth spanning the entire buffer (0.5s at 96000 bytes / 192000 bytes per sec). Subtracting it raw made the clock jitter by up to half a second and the tick alternately held and skipped frames chasing it. An EMA (alpha 0.02) plus a monotonic clamp measurably improved it on 4K ProRes 422 HQ: corrections **35 → 22** over a 7s clip, frames presented **152 → 158**, rate **87.4% → 91.4%** of real time, worst-case sync **80ms → 70ms**. Residual wobble is the 42ms tick against a 41.67ms frame duration; closing it properly needs a real presentation clock and belongs with the GPU renderer pass, not another filter here. **Aug 2026 follow-up**: freezing the latency term at its seeded value instead of sampling was measured against the EMA and came out neutral (rep 9 skip 7 vs rep 8 skip 6, same rate, same drift), so the EMA stays. It is seeded from the configured buffer duration now rather than from the first sample, which reads ~0 against a steady state of nearly a whole buffer. The residual this entry attributes to the tick was actually the two-scheduler bug — see below.
- **Audio owns its own demuxer and decode thread** (Aug 2026): sharing `VideoDecoderFFmpeg`'s `AVFormatContext` would mean locking it against the seek-heavy video path on every packet, and that path is deliberately single-threaded. This does not reopen the async-video-decode question — it is a separate stream with no frame-ordering contract.
- Video playback never skips frames (timer clamps steps to 1 for video) — heavy files slow down rather than drop frames. Deliberate: ordering over rate, except under the audio master clock above.
- Windows ships as **portable ZIP only** — no installer until packaging/playback stabilize (`docs/release-notes-alpha.md`).
- **TRACE IS BETA FROM `v0.2.0-beta.1` (2026-08-15). The named gate was mixed-monitor DPI and it
  closed on hardware**, at `8945894` — `v0.2.0-alpha.1`'s own notes called it *"the main reason
  this release is still alpha rather than beta"*, so promoting the release was reading that
  sentence rather than making a judgement. Shipped with **`TRACE_PLAYBACK_QUEUE` default off**,
  which is the configuration every regression figure in the release was taken on; the notes name
  it as a knob for testers with heavy media, with the measured +9% and the reason depth 1 is
  worse than off. Tag build green with **all four verification steps read individually**
  (run `31919458108`): `20.8 MB` / system-DLLs-only · `FFmpeg detected` + `Audio dependencies
  detected` · `6 required files present, 95.3 MB total` · **`renderer=d3d11 fellback=0
  planar=1`** · `OK - 11 shapes x 4 scale factors`. **`fellback=0` means the runner took the
  HARDWARE path** — the check accepts `d3d11 (warp)` by prefix, so a WARP pass looks identical
  in the step status and different in that line, which is the reason to read the output rather
  than the tick.

  **THE RELEASE STAGE IS NOT IN THE VERSION NUMBER, AND IT LIVES IN THREE PLACES.** CMake's
  `VERSION` field cannot hold a prerelease suffix, so `project(Trace VERSION 0.2.0)` covers both
  the alpha and the beta of this line and the *word* is what distinguishes them. It is a literal
  in `src/app/MainWindow.cpp` three times: `buildIdentity()`'s `Trace %1 (beta)`, the About
  dialog's small print, and the **Report an Issue mail subject**. Missing one leaves the number
  looking right while the build names the wrong stage in the one place a tester quotes back.
  **Verify against the built binary, not the source** — reading the compiled strings out of
  `Trace.exe` is what confirms all three moved and none survived.

  **THE PACKAGE IS `trace-windows-x64` AND CARRIES NO RELEASE STAGE (renamed 2026-08-15, after
  the beta shipped as `trace-alpha-*`).** The handoff enumerated it as *five* references in the
  workflow plus one here; there were **nine in the workflow and twenty across the tree**, so an
  enumeration would have missed four in the file it named. It is `DIST_NAME` at the top of the
  workflow now and is spelled **once** — which is what removes the "one typo away from a broken
  publication" risk that kept the rename out of the release commit, rather than testing for it.

  **Two `trace-alpha` strings must NEVER be renamed and a blind search-and-replace hits both**:
  the remote `https://github.com/bigsbypuglise/trace-alpha` (the repository), and the filename
  `docs/release-notes-alpha.md` (kept so links do not break; it says so in its own first
  paragraph). **Only two of the nine workflow references run on a tag** — the `Compress-Archive`
  and the publish step's `files:` — so an ordinary green branch build cannot speak for them, and
  the ZIP step now asserts its own output by name and size for exactly that reason.
  **Published assets up to `v0.2.0-beta.1` were not renamed retroactively.**
- **Scrub shows a reduced-resolution preview above 1920px wide** (July 2026, threshold corrected Aug 2026, target size corrected Aug 2026 — see the byte-budget entry above): sws conversion dominates 4K frame cost. Half resolution is now a *ceiling*; the actual target is the size the viewer will draw at. The landing frame (slider release) is always full-res accurate via Step mode. Preview-resolution frames **do** enter the cache but are tagged `previewRes`, and `tryReverseCache` refuses them for anything but a Scrub — the old rule forced cache fills to full res so they could serve a step, which paid double for entries that were declined anyway. Don't "fix" scrub softness by removing this at 4K — fix it by making conversion faster.

  **The threshold is `> 1920`, not `>= 1920`.** At exactly 1080p halving was a *pessimisation*: a full-res convert is 1920x1080 → 1920x1080, which sws does unscaled, while halving adds a 1920→960 resample costing more than the smaller output saves. Measured on the same file in one session: full-res `sws 0.57/0.72ms` against half-res `sws 2.50/5.07ms`. 1080p was being caught by a rule written for 4K, which throttled the drag shuttle to roughly a third of its rate *and* showed a soft preview for it. Correcting the bound took shuttle cost 3.60 → **1.79ms/frame** at 1080p with full-res previews. **4K H.264 has not been re-measured** — full-res 4K 8-bit conversion is only ~2.9ms, so the same inversion may apply there; 4K ProRes 10-bit (~15ms) is where halving is clearly right.
- **Transport widgets must not take keyboard focus** (`setFocusPolicy(Qt::NoFocus)` on the slider): keyboard belongs to frame stepping and J-K-L. If a new widget steals arrows/space, this is why.
- **A slider press is a jump; only movement after it shuttles** (Aug 2026, `c3335ec`): the slider does an absolute set on a groove click, so the value arrives before the pointer has moved anywhere — and the drag shuttle then walked every frame between the playhead and the click target before the release landed it. On 4K ProRes 4444 that is a run of ~25ms decodes in front of a frame the user pointed straight at, which is what "slow to lock onto the selected frame" was. `scrubJumpPending_` makes the first flush after a press land exactly through Step. Measured from a cold playhead, a click is now one seek and one decode, `walk 0f`.

  The release also skips re-decoding a frame the press already landed (`scrubShownExact_`), since a click arrives as press-then-release on the same value. **Only when the frame is known exact** — a shuttled or preview-resolution frame must still be re-landed, so this can never leave a soft picture as the landing.
- **Play at the end of a file restarts it** (Aug 2026, `c3335ec`): playback stops on the last frame and leaves it there, so a second Play had nothing to advance to and the button read as dead. `playbackAtEnd_` is set both when the playhead reaches `maxFrame` and when the decoder is exhausted at the tail — that is the end of the file as far as the viewer is concerned even when the frame count disagrees — and is cleared in `refreshHud` by any move off the frame playback stopped on, which is the one place every transport action passes through. The rewind happens in the Play action, not in the tick, so the playhead is never moved while stopping.
- **The timeline slider does absolute seek on click** (Aug 2026, `9a214f2`): Qt's default binds groove-click to `SH_Slider_PageSetButtons`, so clicking the track nudged the playhead by `pageStep` (10) frames instead of going there. A `QProxyStyle` swaps `SH_Slider_AbsoluteSetButtons`/`SH_Slider_PageSetButtons` so QSlider's own machinery maps the click and continues into a drag — don't hand-roll the position math, the style path keeps groove/handle geometry and RTL correct.
- **Conversion contexts are a small LRU set, not one shared context** (Aug 2026, `5e57d86`): three configurations are live during a scrub cycle — full-res accurate (exact frame), full-res fast (cache fills), half-res fast (drag preview). One shared context rebuilt on every alternation, costing **~8–9ms on every slider release**. Keying two slots on geometry alone is *not* enough (the full slot still thrashes on the fast/accurate flag — measured 12 rebuilds over three drags). Four slots keyed on the complete tuple settle to 3–4 rebuilds ever, then pure reuse.
- **The frame cache is consulted in both directions for random access** (Aug 2026, `0728db3`): it used to be checked only when a request moved backward, so a forward scrub onto a frame decoded moments earlier still seeked and re-walked the GOP. Sequential playback is still excluded — it must keep advancing the decoder rather than being served from behind. Biggest single win: a slider click issues press *and* release for the same frame, and the release is now a cache hit instead of a second full GOP walk.
- **Cache eviction stays FIFO** (Aug 2026, `9513965`): LRU (promote-on-hit) was prototyped and measured against FIFO with capacity, fill policy and lookup held identical. On 4K H.264 — the only place the cache actually fills and evicts — both gave **hit 60.0% (9/15), 11 inserts, 5 evictions**; LRU recorded 9 promotions and changed nothing, because the scrub working set exceeds capacity rather than a hot subset being evicted early. At 1080p nothing is evicted at all (17/24 occupancy on a 9-target pattern), so the policy is unreachable. Don't re-try LRU without first making the working set smaller than capacity.
- **The viewer filters the fit-to-window resample** (Aug 2026): `SmoothPixmapTransform` was off, so any window not exactly the source resolution point-sampled the frame — dropping whole pixel rows and stair-stepping every diagonal. A tester caught it against QuickTime. Filtering is on whenever the frame is resampled and **off at 1:1**, where it could only soften pixels being inspected. `TRACE_NEAREST_SCALE=1` restores the old path; HUD `display` shows `1:1` / `filtered` / `NEAREST`. Note Qt's raster filter is bilinear: fine to ~2x downscale, weaker beyond it. Qt's raster filter is bilinear: fine to ~2x downscale, weaker beyond it. **Scrub previews no longer go through it** — they convert to the display size in swscale and draw 1:1 (see the byte-budget entry above), which is both cheaper and higher quality. The **landing frame still does**: a Step converts full-res and Qt scales it, which on the validation window is a 6.4x downscale. Measured, preview and landing local contrast are within 0.7%, so there is no visible inversion today — but if landing quality is ever the complaint, converting Step to display size too is the fix, and the cache-clear-on-resize machinery it needs already exists.
- **swscale is told the source colorimetry** (Aug 2026): it was never given any, so it used its BT.601 default for every file — the wrong matrix for essentially everything Trace opens, which flattened skin tones and shifted saturated colour. Range was likewise assumed limited, washing out full-range files. Matrix and range are now read off the decoded frame (falling back to the codec context, then to the standard "HD and up is 709" heuristic) and applied per sws slot via `sws_setColorspaceDetails`, which recomputes tables rather than rebuilding a context. Colour details are slot state, not per-call — they are part of what a slot caches. HUD `color` line shows the matrix (with `*` when inferred) and range. **BT.2020 gets the right matrix but no tonemap**: HDR/PQ content will still look wrong, and that is a known gap, not a regression.
- **Cross-platform picture comparisons are not evidence on their own** (Aug 2026): macOS QuickTime colour-manages to the display profile; Trace on Windows does not. Any Mac-vs-Windows screenshot pair shows a tint difference for that reason alone. Ask for same-machine, same-display comparisons before treating a colour report as a bug.
- **Paint pacing during a drag is a dead end — measured twice, rejected twice** (Aug 2026, `5daa5ce`). The theory each time was that a shuttle paints faster than the panel refreshes, so most frames are overwritten unseen and the motion arrives as bursts. The theory is *true* — 616 of 627 paints at 4K, 98%, land inside one refresh interval — and fixing it buys nothing.

  First attempt broke out of the walk and re-armed a timer per frame, which throttled the decoder as well: 151 paints against 631, ~45/sec, and a fast drag could not finish a sweep. Second attempt only declines repaints and never interrupts the walk, so catch-up speed is untouched. Measured:

  | | wasted paints | **stalls** | max gap |
  |---|---|---|---|
  | 4K H.264 | 98% → 43% | **7 → 8** | 102 → 116ms |
  | 1080p | 97% → 26% | **21 → 34** | 78 → 85ms |

  A paint costs 0.23–0.36ms, so ~600 wasted paints is ~200ms across an entire multi-gesture run — not a stutter. Stall count was unchanged at 4K and clearly worse at 1080p. `TRACE_SCRUB_PACE` keeps the better mechanism available (0 = off, the default; 1.0 = one frame per refresh).

  **The point to carry forward: burstiness is not what a drag feels like, stalls are** — the 30–116ms gaps where a cache miss forces a seek and a GOP walk. No paint scheduling can reach those. Don't return to pacing; make misses rarer.
- **`currentFrame_` is not where the decoder is** (Aug 2026, `2523d77`): a cache hit sets it without advancing the decoder, so the two diverge by however far a cache-served drag ran. The seek decision used to ask whether the *request* was sequential (`frameIndex == currentFrame_ + 1`) and skip the seek if so — which meant that after running to the end of a file, dragging back through cache hits, then dragging forward again, the request looked perfectly sequential while the decoder sat at EOF with its drain packet sent. `decodeUntilTarget` returned false at its drained check, which is *above* the `staleSuccessPrevented` counter, so the HUD read `stale-blocked 0` while "No decodable frame at target position" appeared on screen. Always ask whether the **decoder** can reach the frame (`frameIndex > lastDecodedFrame`); `lastDecodedFrame` only moves on an actual decode. Reproduced in 4 of 8 scripted-reversal captures, zero in 24 after. Always reachable, but it needed a run of backward cache hits — raising the 4K hit rate from ~0% to ~88% is what made it findable. HUD `recov N` counts the backstop retry and should stay 0.
- **Approximate scrub previews are rejected** (Aug 2026): capping the GOP walk during drag was prototyped twice and never shipped. On a 30-frame GOP a cap of 8 shows a frame ~21 frames (~0.9s) behind the pointer — unacceptable for a review tool. Exact frame identity during scrub is the constraint; make the cache better instead.

## LucidLink / high-latency storage — measured Aug 2026

Trace reads media through its own `AVIOContext` (`MediaIoSource`), sized to FFmpeg's own default so it measures rather than changes the read pattern. Counters are per phase (Open / Playback / Seek) and must never be averaged together.

- **The read pattern was never the problem.** Forward playback is **100% sequential with zero seeks** on every source measured, local and remote, and FFmpeg bypasses its own buffer for requests larger than it so one video packet is one read (2.4 MB on 4K ProRes 422, 130 KB on 1080p H.264, 11.5 MB on a 9K ProRes 4444 plate). **Phase 5 of the brief — a custom buffered chunk layer — is not warranted and was not built.**
- **Reads are synchronous on the UI thread, and that is the whole problem.** Cold 3.2 GB / 4497 Mbps ProRes 4444 from LucidLink: **15.2 s of 20.8 s wall time blocked inside `QFile::read`**, handler 557 ms/frame, `outside` 0.36 ms — the UI thread is pinned, so the app is frozen, not merely slow.
- **Cold vs warm is the whole story** (same file, same run order): streaminfo **408 → 4.8 ms**, read latency **81.5 → 0.865 ms**, playback **2.41 → 8.84 fps**, stalls **45 (15,214 ms) → 3 (577 ms)**. Warm, the file is CPU-bound (108 ms/frame decode+convert on a 9216×3164 plate); cold, it is purely I/O-bound.
- **Read-ahead is warranted but is not a bandwidth machine.** Cold LucidLink delivered ~610 Mbps measured against read time. A 4497 Mbps file cannot stream cold no matter how it is buffered — read-ahead can only recover the decode-time gap where the link currently sits idle (~250 ms of every 557 ms frame). The real beneficiaries are the **~100–600 Mbps** class (4K ProRes 422 HQ and friends), which sit just under what cold delivery achieves. Do not promise that read-ahead fixes 4 Gbps plates.
- **1080p/low-bitrate remote playback is already fine**: 12.5 Mbps LucidLink file showed **zero stalls, 0.029 ms reads, 95.2% of real time vs 95.3% local**. Don't optimise it.
- **`probesize` does nothing; `analyzeduration` is the entire open-time win** — and only below 250 ms. At 100 ms: 1080p H.264 streaminfo 23.5 → 10.4 ms, 4K H.264 69.7 → 36.5 ms, LucidLink open 96.5 → 49.9 ms, with all 14 metadata fields identical across 25 opens. **Not shipped**: every test file is a well-formed professional export where fps comes from the container. The checklist's "variable/timing-uneven source (phone/screen capture)" case — where fps must be inferred from packets, i.e. exactly what a short window breaks — was unavailable. Knobs are `TRACE_PROBESIZE` / `TRACE_ANALYZEDURATION`; defaults unchanged.
- Storage classification is cached per volume (`7.9 → 0.0 ms` on the second open in a session). LucidLink is **not** `DRIVE_REMOTE` — it presents as `DRIVE_FIXED`/NTFS and is recognised by advertising petabyte capacity with `free == total`. Never keyed on drive letter or volume label, and never by writing a probe file.
- `TRACE_OPEN_LOG=1` writes one line per open (fps to 6dp, exact frame count, time base, colour metadata) so probe experiments are validated on exact values.

### Responsive I/O (shipped, `8b47e08`)

Remote reads no longer run on the UI thread. A dedicated worker performs the blocking read while the calling thread pumps the event loop; **longest UI-thread block went from ~1067 ms to ~5.9 ms**. Three things matter if you touch this:

- **Local keeps the direct synchronous read.** No worker, no handoff. Measured identical (open times within noise, 4K ProRes 94.4% of real time, `buffering 0 / waiting 0ms` proving the async path is never entered locally). Don't "unify" the two paths.
- **Cancellation never abandons a read.** The destination buffer belongs to FFmpeg and is being written; returning early would hand the decoder a buffer still in flight. A superseded read runs to completion and is *reported* stale, and the decode spanning it is discarded. Latest-target-wins survives.
- **`storageBusy_` re-entrancy guard is load-bearing.** Pumping events inside a decoder call lets a timer tick or key press re-enter FFmpeg mid-read. Every path that drives the decoder checks it and defers; scrub re-arms rather than dropping. This is the first place to look if odd frame-order or crash reports appear.

### Read-ahead — TRIED TWICE, NOT SHIPPED, both measured worse

Reverted, uncommitted. Benchmarked on 2160×3840 ProRes 4444 @ 1013 Mbps from LucidLink, using an injected per-read delay to reproduce the cold profile repeatably (a real cold cache is a one-shot — once read, a file is warm for good; the injector reproduced cold within 3%: 11.63 fps vs a real 11.35).

| | fps | read lat | throughput | seeks | seq | buffer |
|---|---|---|---|---|---|---|
| control (off) | **11.63** | 54.9 ms | 762 Mbps | 2 | 99.2% | — |
| v1, 8 MB | 9.60 | 72.8 ms | 574 Mbps | 2 | 99.2% | hit 0 / miss 128 |
| v2, 16 MB | **7.02** | **19.9 ms** | **1218 Mbps** | **25** | **91.1%** | hit 218 / miss 5 |

- **v1 failed outright**: the decoder also issued its own reads, rebasing the buffer while fills were in flight, so every fill was discarded (0 hits) and speculative fills queued ahead of the read the decoder was blocked on.
- **v2 fixed the mechanism** (worker as sole reader) — hit rate 218/223, latency down 2.8×, throughput up 60% — **and playback still got worse**. Cause: serving `min(requested, available)` fragmented reads (123 @ 5105 KB → 218 @ 2967 KB), dropped sequentiality to 91.1%, and drove demuxer seeks 2 → 25, each discarding the buffer.
- **Next experiment before anything else**: only satisfy FFmpeg's read callback when the *complete* requested byte count is buffered (except at EOF), so read sizes stay ~5 MB and the demuxer never repositions. Benchmark before committing.
- **Buffering cannot beat bandwidth.** Cold delivers ~600–800 Mbps. A 4.5 Gbps 9K file cannot be made real time by any buffer; even a perfect read-ahead on the 1013 Mbps file tops out near 19 fps. Target case is realistically **~100–600 Mbps** studio review media (4K ProRes 422/HQ).
- The benchmark needs an injected-latency knob to be repeatable. Rebuild it if resuming.

## Roadmap (rough priority)

1. ~~4K ProRes 4444 playback is at the limit of the sync design~~ **Resolved 2026-08-07**: the ~38ms figure was dominated by a bug, not by codec cost. `QImage::bits()` was detaching — a full ~37.7MB deep copy of the previous frame, every frame, of data `sws_scale` then overwrote — because the viewer and reverse cache still referenced the buffer. A recycling conversion-buffer pool removed it. Playback is now **~23.7fps (~99% of real time), late 0**, with per-frame work at ~33ms against a 41.67ms budget, i.e. **~8ms of idle headroom**. The remaining gap to 24.000 is *not* CPU cost: video presents one frame per timer tick, `round(1000/24) = 42ms`, so the hard ceiling is **1000/42 = 23.81fps** and we measure 99.4–99.8% of it. Exact 24.000 needs a presentation clock with sub-ms resolution (vsync / waitable swapchain) — fold it into the GPU renderer pass. Three scheduler alternatives were measured and reverted; see the comparison table at the timer setup in `MainWindow.cpp` and don't re-try them.
2. ~~**H.264 scrub is cache-bound, not decode-bound**~~ **Largely resolved 2026-08-07 by the drag shuttle** — see the scrub entries in Decisions. The measurement stands (seek costs 1–3ms; exact scrub cost scales with the GOP walk, 0f ≈ 34ms to 29f ≈ 61ms; a cache hit serves in 0.33–0.55ms) but the conclusion drawn from it does not: the fix was to stop *seeking* during a drag rather than to make the cache better. Walking forward costs ~1.8ms/frame against ~34–61ms for a seek-and-walk, so a drag that used to pay a seek per update now pays 2 seeks for an entire sweep. Levers (a)–(c) from the original entry are no longer the priority. **What is left here is backward dragging**, which still seeks past the reverse-cache window — folded into item 6.
3. **GPU-backed presentation / D3D11 — GATES B, C and E ALL PASSED; `d3d11` is the DEFAULT renderer as of 2026-08-10.** The plan and every decision live in `docs/gpu-initiative-plan.md`; read that first, it wins over anything summarised here. Steps 2–6 are committed and validated: `VideoFrame` at the four frame seams (`03d840e`), the `VideoRenderer` boundary (`5765c19`), generation plumbing (`75a3412`), the async scrub worker (`ff55d4e` + `f77d472`), sampled drag preview (§15), play/pause across a drag (§16), and a **native D3D11 surface** (§17) — a DXGI flip-model swapchain on a child HWND presenting swscale's BGRA. **`TRACE_RENDERER=d3d11` is opt-in; `cpu` is the default and stays so until GATE E.**

   **GATE B is pending on exactly two things** (plan §20.2), and neither is a rendering fault: **(a) human visual review** — every number says the right frame is at the right size, none says the picture looks right, and 4K ProRes 422 HQ is the bar; **(b) the HUD logical/device-pixel unit bug** — at 1.5x DPI the CPU backend reports `display 640x360` (logical) and D3D11 reports `display 960x540` (device) for the *same* on-screen rectangle. D3D11's is honest. They agree at dpr 1, which is why it hid until §18.2.

   Everything else passed: CPU/GPU pixels are effectively identical at the shipping DPI (max channel delta 1–2 on 4K H.264, ProRes 422 HQ and 4444), playback 98.3% either way, scrub reversals `rev-hit 97.9%` / `stalls 2 of 394` against `2 of 402`, resize + maximize + minimize + fullscreen all hold content, exactly one surface child window, clean shutdown in ~105ms.

   **Two open items to carry, both recorded in plan §20.3–20.4.** At **150% scaling** CPU and D3D11 differ on 3.9% of the video band, max channel delta 75 — Qt's raster bilinear against the D3D11 sampler. Not geometry, identity or colour; it needs an eye. And note it is *larger* at 1.5x (4x downscale) than at dpr 1 (6x downscale), which is the wrong direction for a filter-quality explanation — don't accept a hand-wave. ~~**Real mixed-monitor DPI is untested**~~ **— CLOSED 2026-08-14 (§20.4).** It was true when written: the box had one display, so `QT_SCALE_FACTOR` was all that ran. A second display was connected on 2026-08-14 and monitor-to-monitor moves, per-monitor DPI changes and fullscreen on a secondary display have all now executed, on both backends. **Note the §20.3 item above is NOT closed with it** — the 150% cross-backend band difference was measured under `QT_SCALE_FACTOR`, and the 2026-08-14 pass compared *framing* across backends (identical to the pixel) rather than re-running the band diff at real 150%.

   **GATE B is PASSED — owner visual sign-off, 2026-08-09** (plan §20.2, §17.5 item 2). CPU and D3D11 are visually equivalent in fit-to-window and fullscreen, and the 150% case is accepted with no meaningful softness, scaling artifacts, colour or framing difference — so §20.3 closes as acceptable rather than as a defect. ProRes 4444 scrub passed too. Verdict: proceed with D3D11. **`cpu` stayed the default until GATE E**; the sign-off was on the rendering, not on which backend ships enabled. GATE E has since passed and the default flipped to `d3d11` (plan §25). The two measurable blockers were fixed first: the HUD unit bug (`58ec879`) and the fractional-DPI rect divergence (`ddb38ca`), where the downscale ratio turned out to be a confound.

   **GATE C is done** (`e8566a4`): planar YUV upload with the matrix in the shader, confirmed against swscale at 8/10/12 bits, conversion cost down 2.5–4.1x, scrub unchanged.

   **Step 8 is CLOSED as answered-no and step 9 is DONE** (2026-08-10, plan §27/§28). Step 8's premise had expired — GATE B's own lazy creation already reuses everything (`tex 3` across 261 frames, `tex 4` across a 406-paint drag) and the residual upload is memcpy bandwidth at 16.3 GB/s. Step 9 was the opposite: a real, never-measured defect, and bigger than §9 described because *every* full-resolution frame went through one 2x2 sampler tap, not just the landing. Fixed, no measurable cost, **owner sign-off on the picture 2026-08-10**. **Step 10 (10-bit output) is the only deferred GPU item left, and no GPU item has an open owner question.**

   **GATE E is PASSED (2026-08-09, `e2b8655`, owner sign-off).** It was pulled ahead of steps 8–10 by owner decision and split in two: **step 11a (E1, the absolute-deadline scheduler) shipped and is what fixed the stutter**; **step 11b (E2, DXGI vsync snapping) is not built and is stopped**. Steps 8, 9 and 10 remain deferred, not cancelled. The reasoning is §23.5: locked real-time playback is priority #1, §23 measured the residual stutter as the universal integer-tick beat which only GATE E fixes, and §23.4 measured headroom — all steps 8–10 buy — as no longer the binding constraint on 4444 once the planar path is on. No technical dependency runs from 8–10 into 11; the flip-model swapchain landed at GATE B.

   **Four things from that design worth carrying even if it is rewritten.** (a) **Locking the wake does not lock the present** — `setFrame` calls `update()`, so the paint and `Present` run after the tick handler returns, and present intervals therefore carry `handler_k − handler_{k−1}` directly; 4444's 25→37ms handler spread is ±3 refreshes on this panel. Any design that only reschedules the timer inherits that. (b) **The waitable swapchain is the wrong instrument** — it answers "may I queue another frame", not "when is the next vblank"; at sync interval 0 it carries no phase, at ≥1 it forces a present every refresh, and blocking on it from the UI thread is the mistake `8b47e08` fixed. `DwmGetCompositionTimingInfo` is a read, never a wait, and is renderer-independent. (c) **Measure the panel's true refresh** — §22.8 recorded 239Hz, and a "240Hz" mode is often 239.76, on which 24.000fps content cannot have constant cadence in any player while 23.976 maps exactly. GATE E replaces a beat Trace creates with whatever the display imposes; it cannot promise zero without that number. (d) **E1 is a near-relative of the already-rejected "adaptive single-shot per frame"** (comparison table at the timer setup) — but that table is all presented-rate, which §23.1 proved blind to the beat, the starvation objection was about a 35ms handler that GATE C has since changed, and the reverted code is not in git history so the 0.7% cannot be attributed by reading. Re-measure on the cadence distribution; do not argue it down from the table.

   **One thing to look at before GATE E**: 4K H.264 reversals now measure ~44 stalls of ~375 on *both* renderers, against the "2 of 394" recorded at §17.4 on the same file and gesture. A control build of the preceding commit reads the same, so it predates this session's changes — but stalls are the metric the scrub complaints live in, and a 20x move deserves an hour. (Largely settled at §22.8: cache depth is a function of window size and dominates; quote `win WxH` with any stall number.)

   **The overlay question is settled and that work is stopped** (plan §19, §20.1). Ordinary Qt child widgets over the child HWND are neither visible nor hit-testable; every native-window variant loses translucency. **Renderer-composited translucency works** — real alpha over the video, full native input, keyboard staying with Qt via `MA_NOACTIVATE`, and **no measured playback cost** (98.3%, 120/120 with the overlay held visible through a 9s 4K run). The child HWND stays; **`WA_PaintOnScreen` is not promoted** — it works on this build but Qt documents it X11-only. `TRACE_OVERLAY_COMPOSITED=1` is a **disposable spike with placeholder art**, off by default, and it announces itself on stderr. No further overlay/interface work until GPU integration is complete.

4. **LucidLink read-ahead — a third design is BUILT, default off, correctness-verified, and
   explicitly NOT validated against real remote storage (2026-08-15, unattended session, no live
   remote access). Full writeup: `docs/lucidlink-readahead-v3.md`.** `TRACE_IO_READAHEAD=1` in
   `MediaIoSource`: a fill-ahead buffer, full-request-or-wait serving (never fragmented except
   when a single read exceeds capacity), the exact "next experiment" the two prior (reverted,
   uncommitted) attempts' postmortem proposed. Two new synthetic knobs for reproducing a slow-link
   read pattern on **local** media: `TRACE_IO_INJECT_KBPS` (bandwidth cap) and
   `TRACE_IO_INJECT_DELAY_MS` (fixed per-read latency — the model that actually matches what
   read-ahead is for, and what a prior session's "injected per-read delay" note describes).

   **Two hang-class bugs were found by testing small-capacity-vs-large-packet cases, not by
   review**, and both are fixed: the default 256KB fill chunk cost 7.7s to fill a 24MB buffer
   under an 80ms/call latency (fixed: 4MB default, `TRACE_IO_READAHEAD_CHUNK_KB` to tune); and a
   single FFmpeg read bigger than the buffer's capacity could wait forever for room the paused
   fill loop would never free, with a second boundary case surviving the first fix when
   `raChunkBytes == raCapacityBytes` exactly (both fixed; reproduced and confirmed closed with
   `TRACE_IO_READAHEAD_MB=1` against ProRes 4444's ~2.4MB packets — 8s+ hang before, 300ms clean
   close after).

   **Correctness confirmed pixel-identical** (`abdiff.ps1`, 0% differing) against the legacy path
   at a stepped frame and across a mixed forward/backward sequence exercising multiple buffer
   rebases under injected latency. **One harness lesson**: the first mixed-direction attempt used
   a rapid 15ms key-press cadence and read 99.998% different — a timing artifact (rapid presses
   coalesce, and differing I/O latency between configurations resolved a different number of them
   before landing), not a correctness bug; a 300ms cadence made both land on the identical frame
   and the diff read 0%.

   **Synthetic results, `TRACE_RT_DROP=0`, 4K ProRes 422 HQ, 10s play**: at 20ms injected
   per-read latency, read-ahead cuts average read latency 22.4 → 0.79ms (98.6% instant buffer
   hits) with the file already finishing inside the window either way; at 80ms, read-ahead
   delivers roughly double the data in the same wall-clock window (458.8MB vs 230.9MB) by hiding
   most read latency behind decode/paint. `TRACE_RT_DROP=0` was necessary to isolate this from a
   real, separate interaction: without it, the real-time drop mechanism's jump-ahead on a missed
   deadline is genuine random access on intra-only media, and a read-ahead rebase correctly
   discards the buffer on every one of those jumps, which is a compound of two mechanisms rather
   than what this experiment measures.

   **Do not quote any of the above as LucidLink performance.** No live remote access this session;
   the absolute latency/bandwidth figures are a relative on/off comparison under an injected delay
   this session had no way to calibrate against a real cold read (a prior session's calibration —
   "11.63 vs 11.35 fps, 3% off" — is recorded in this file but was not reproducible without a live
   mount). Closing this out needs either a nominated file on the read-only `V:\` mount or a real
   cold-read figure from Anj to calibrate the synthetic knobs. `TRACE_IO_READAHEAD` stays off by
   default; flipping it is an owner decision that should follow real validation.
5. ~~**Audio, first pass — needs validation on Windows**~~ **Validated and fixed 2026-08-07**, on the local Windows toolchain against the `Trace_Testing_Assets` set. The three open questions are answered: (a) the device-latency correction is adequate — a fixed term measured neutral against the EMA, and residual sync is ≤62ms worst case at a 100ms buffer; (b) no underruns during playback on any file including 4K ProRes 422 HQ — the only silence padding is at end of stream once the audio track runs out, and the ring is now derived at ≥2x the device buffer rather than a fixed 0.5s; (c) the bounded catch-up no longer fires at all in normal playback — **skips are 0** on every file measured. Results: 1080p H.264 **99.1%** of real time (240/240 frames), 4K H.264 **98.3%** (120/120), 4K ProRes 422 HQ **98.4%** (168/168, against a recorded baseline of 164 frames / 95.0%), 4K ProRes 4444 no-audio control **98.3%** (261/261). Remaining residual is 3–5 *holds* per 10s clip with no frame dropped, which is the 41ms tick against a 41.667ms frame — a presentation-clock problem, and item 3's to solve. **Still not done: the LucidLink regression** (`start()` now takes a bounded ≤150ms UI-thread wait to prime the ring; ~10–13ms locally, unmeasured on a cold remote source) and J-K-L off-speed audio, then scrub audio.
6. ~~**4K scrub throughput — the cache is sized in the wrong currency**~~ **Resolved 2026-08-07** (`b5a56af`), except for ProRes 4444. Eviction is by bytes now and previews convert to the displayed size; see the scrub entries in Decisions for the measured table. 4K H.264 backward went 31.3 → 0.69ms/frame, ProRes 422 backward 13.1 → 7.5ms, and everything except 4444 reaches `lag 0-1f` on a 1.5s full-clip sweep.

   **What is left is 4K ProRes 4444, and it is decode-bound**: 15.4ms of its 17.7ms/frame is the ProRes decoder itself, so no cache or conversion work can reach it and FFmpeg's ProRes decoder has no `lowres` path. ~56 frames/sec, about 2.3x playback, against the owner's "~4x on a fast drag". The only remaining levers are skipping frames — which the shuttle deliberately never does — or decoding off the UI thread. Treat "4x on 4444" as a product decision to take explicitly rather than a bug to fix.

   Reverse *playback* (as opposed to dragging) beyond the cache is still the same underlying problem — H.264 needs GOP-aware backward buffering.
7. EXR / image-sequence review polish, OCIO display transform (TODO marker in `StillImageLoader.cpp`). **EXR does not open today**: OpenImageIO is not installed in vcpkg and not built in CI, so `TRACE_WITH_OIIO` is undefined in both.

## Where scrub stands (2026-08-07, second session)

Forward dragging was already signed off ("feeling really nice"). This session
addressed the owner's report that **ProRes HQ, ProRes 4444, 4K MP4 and 4K 60fps
MP4 were all slow to respond to fast drags**, that 4K MP4 backward was
"stuttery and slow", and that clicking the timeline on 4K ProRes 4444 was slow
to lock on. 1080p MP4 and the PNG sequence were reported good and are unchanged.

**The product spec, in the owner's words:** click jumps to a point; dragging
displays every frame consecutively and never jumps; a fast drag should feel
like ~4x playback, a medium drag ~2x, easing to a stop; the slider should
always feel smooth. Frames are never skipped during a drag at any speed --
overrun shows up as lag and is walked off. All of that still holds.

**Where each case now sits.** 1.5s continuous sweeps across the whole clip in a
1280x760 window; ms/frame is the shuttle rate, lag is frames behind the pointer
at the moment it stops:

| file | before | after |
|---|---|---|
| 4K H.264 backward | 31.32ms, lag 50f, 57.1% hits | **0.69ms, lag 0f, 87.9% hits** |
| 4K H.264 forward | 10.95ms, lag 2f | 11.33ms, lag 1f |
| 4K 60fps H.264 backward | — | **2.87ms, lag 0f, 90.4% hits** |
| 4K ProRes 422 HQ backward | 13.08ms, lag 49f, 0% hits | **7.50ms, lag 1f** |
| 4K ProRes 422 HQ forward | 12.09ms, lag 45f | **8.91ms, lag 1f** |
| 4K ProRes 4444 backward | 25.38ms, lag 171f | 17.71ms, lag 156f |
| 4K ProRes 4444 forward | — | 18.24ms, lag 156f |
| 1080p H.264 (untouched path) | — | 2.82ms, lag 0f |

**The one case still short of spec is 4K ProRes 4444**, and it is decode-bound:
15.4ms of its 17.7ms/frame is FFmpeg's ProRes decoder, which has no `lowres`
path. That is ~2.3x playback against the owner's ~4x. Nothing in the cache or
the conversion path can reach it. The honest options are to decode off the UI
thread (which reopens the async-decode question the project has twice reverted)
or to skip frames on the heaviest media as an explicit product decision. Do not
promise 4x on 4444 without one of those.

**4K ProRes 422 HQ scrub is the quality bar** (owner, 2026-08-07): "that 4K
ProRes HQ is for sure our new north, ideally all media would function/scrub
just like this." Use it as the reference when judging any other format --
compare against it directly rather than against a target number.

What makes it work, so the bar is reproducible rather than lucky: **~5-6ms per
frame** (decode 3.6 + sws 1.3 + paint 0.1), every frame a keyframe so a seek
costs nothing and a miss has no GOP walk, and previews converting straight to
the displayed size. Three of those four are properties of the codec; the fourth
was the fix.

Reaching the bar elsewhere: **4K H.264 and 4K 60fps** decode faster than ProRes
(0.5ms) but are long-GOP, so a cache miss costs a seek plus a GOP walk -- that
is a prefetch problem and is reachable. **1080p** is already cheaper per frame
and has the same stall problem. **ProRes 4444 is the honest exception**: decode
alone is 15.4ms, 4x the whole 422 HQ frame budget, and no cache or conversion
work can touch it.

**Owner testing, after the throughput work (2026-08-07):** 4K ProRes 422 HQ
**signed off** -- "feeling really nice". 1080p MP4 "closest right now" but
backward still "a lil glitchy". 4K ProRes 4444 "still very slow" (expected,
decode-bound). 4K MP4 "decent" but threw a decode error on fast scrub (fixed,
`2523d77`). Overall verdict was that the throughput gain did not convert into
the smoothness expected -- which was correct, and is the entry below.

**Owner test after the async scrub worker (2026-08-08, at `f77d472`): SIGNED
OFF across the whole test set.** "Big improvements across the board", on all
seven files -- so 4K ProRes 422 HQ holds as the bar, and every case that had a
complaint against it improved.

**Read that carefully before concluding anything about throughput, because the
throughput did not change.** 4K ProRes 4444 shuttles at 15.72ms/frame against
17.71ms before -- inside the noise -- and it is still decode-bound with no
`lowres` path in FFmpeg's ProRes decoder. What improved on the heavy files is
responsiveness and *handle tracking*, and on 4444 the second one is probably the
larger part: the picture lags ~156 frames there, which is exactly how far the
slider handle was being yanked back from the pointer on every HUD refresh. The
file that felt worst was the file the yank hurt most.

So: **"4x on 4444" is still an open product decision, not a solved problem.**
The remaining honest options are unchanged -- decode off the UI thread in a way
that produces more than one frame per request, or skip frames on the heaviest
media as an explicit choice. Do not let this sign-off be read as retiring that
question.

**Owner re-test after the GPU-initiative refactor (2026-08-07, at `75a3412`):**
4K ProRes 422 HQ **still feeling great** -- the quality bar held across four
commits that rewrote frame ownership, the conversion buffers and the paint path.
This matters as a validation-coverage note, not just a result: everything
measured on this machine was the 1080p H.264 clip, so the reference format was
confirmed by the owner rather than by the harness. **Any future change to the
frame or render path needs the same split** -- the numbers say whether the
mechanism still works, the owner says whether the bar still holds, and 1080p
H.264 alone cannot answer the second.

**The measurement mistake worth not repeating:** throughput (`shuttle ms/f`)
and `lag` were both excellent on files that felt bad. They say how many frames
were produced and how far behind the pointer the picture is; they say nothing
about *when* frames land, which is what smoothness is. The `smooth` HUD line
added in `5daa5ce` measures the interval between consecutive paints. Reach for
it first on any "feels wrong" report -- and note that a drag can score
perfectly on lag while stalling for 100ms.

**Known open items, in the order they are likely worth attacking:**

1. ~~**Stalls are the stutter, and Gate D did not remove them.**~~ **CLOSED
   2026-08-10 with owner sign-off** (plan §26, §26.6). Two things were wrong with this entry. The
   *count* was mostly an artifact -- `stalls` is measured against the display
   refresh and this box's mode changed between sessions, so the same run reads
   51 or 3 (see the `stalls`/`hitch` entry in Decisions). And the *fix* named
   here was declined: directional prefetch has no idle worker time to spend
   (§15.3), and supply is still 55-67% on the files that hitch, so that holds.

   What the misses actually needed was cache bytes. 192 → 384MB took 1080p
   `hitch 8 → 2` and 4K H.264 `hitch 3 → 1`, with worst gap 169.6 → 80ms. The
   diagnosis in the original entry -- "cache misses forcing a seek plus a GOP
   walk" -- was right; the mechanism proposed to fix it was not.

   **The owner's subjective scrub test on the finished build PASSED** (2026-08-10,
   plan §26.6): the picture feels good on the 384MB cache with `d3d11` default.
   That was the last open half of this item — fourth time the project has needed
   the split between "every figure improved" and "the bar holds", and the fourth
   time only the owner could answer the second. Mechanism, memory footprint,
   verification (plan §26.5) and feel are all now approved. **Nothing about
   scrub stalls is open.** Caveat worth keeping: the session did not record
   whether the test was at the machine or over Parsec, and feel judgements are
   not valid over Parsec — re-take at the panel before leaning on it against a
   future regression.
2. ~~**The slider handle itself trails the pointer**~~ **Fixed 2026-08-08**
   (`f77d472`) -- and the diagnosis in this entry was wrong, which is the part
   worth keeping. It was recorded as event-loop starvation: "the walk loop
   saturates the UI thread, so mouse-move events queue behind decode work".
   Starvation was real and is much reduced, but it was not what moved the
   handle. `syncTransportBar` was writing the *decoded* frame back into the
   slider on every HUD refresh, so the handle was deliberately yanked off the
   pointer several times a second. Two plausible causes for one symptom, and
   the measurement (`ui gap`) was needed to tell them apart -- the theory that
   sounded right accounted for gaps of tens of ms, while the handle was moving
   hundreds of frames.
2b. **The press landing is the largest UI-thread block left in a drag** --
   90-125ms on 4K H.264. It must stay exact, so it cannot be approximated; but
   it could be issued to the worker and awaited with the event loop alive, the
   way remote reads already are.
3. ~~**4K ProRes 4444 fast drag** -- ~2.3x playback against the owner's ~4x~~
   **CLOSED 2026-08-10 by §15, which shipped two days after this was written**
   (plan §29.1). Everything factual here still holds -- 4444 decode is ~23ms/frame
   and FFmpeg's ProRes decoder has no `lowres` path -- but the *conclusion* does
   not. "~2.3x" converts decoder throughput into a drag speed, which is only
   valid while the shuttle presents every frame; sampling made one presented
   frame advance `stride` frames. Both remedies this item proposes were already
   taken: skipping frames on the heaviest media **is** §15, and running the
   worker ahead is directional prefetch, declined at §15.3. Measured at the
   owner's ~4x: `behind 0/6f`, `p2p 26ms`, `delta 0`, **both directions**.
   **Supply below 100% stopped meaning "behind".**
4. ~~**Backward *playback* (not dragging)** beyond the cache is still GOP-walk
   bound on long-GOP H.264.~~ **CLOSED 2026-08-10 with owner sign-off** — the
   bidirectional shuttle (`e9fd236`, `dd21fe9`, `docs/reverse-shuttle-plan.md`).
   The diagnosis in this item was right and the remedy was not what it implied:
   reverse was not short of throughput, it was **bursty**, and the decoder was
   idle 80–93% of the time while missing real time. Queueing the frames off the
   UI thread fixed the cadence; making the speed a **stride** fixed the speed.
   4K H.264 reverse 1x 87.0 → **99.2%** of real time, and accelerated forward —
   which turned out to have the identical fault — went from 4444 delivering
   **1.00x when asked for 2x** to within a few percent of demanded on every rung
   of 2x/5x/10x/30x across all four formats.

   **The measurement pass and the architecture proposal are DONE (2026-08-10) and
   live in `docs/reverse-shuttle-plan.md`.** No implementation was begun, by
   instruction. The baseline is now a full cross-format table taken with
   `scripts/measure/revplay.ps1` rather than §29.3's single row, and §29.3
   reproduces inside it (87.0% against 86.7%). **Every reverse figure recorded
   between GATE E and 2026-08-10 is still void** — it was measuring the J-K-L
   scheduler fault, not the GOP walk.

   Reverse at 1x, % of real time: **4K H.264 87.0 · 1080p 95.4 · 4K 60fps 69.1 ·
   ProRes 422 HQ 99.9 · ProRes 4444 99.7.** At 4x, % of the demanded speed:
   **59.0 · 81.1 · 33.2 · 59.6 · 33.3.** The ladder that exists is 1x/2x/4x;
   5x/10x/30x are not measurable in the app as it stands and are modelled in the
   plan from measured per-frame costs, with the falsification conditions stated.
5. ~~**1080p backward is still "a lil glitchy"**~~ **Very likely closed by the
   384MB cache, 2026-08-10** — but read the qualification. Item 1 was named as
   the likely cause and item 1 is now fixed and signed off: 1080p was the file
   that gained most, `hitch 8 → 2` with `seeks 11 → 4` and hit 96.8 → 98.9%
   (plan §26.3). The owner's sign-off was on the shipping build generally and he
   did **not** enumerate files, so this specific complaint was never separately
   re-confirmed. Treat it as closed, and if 1080p backward is ever raised again,
   know that the mechanism behind the original report has changed underneath it.
6. ~~**The HUD's `target`/`shown` go stale on cache hits**~~ **Fixed 2026-08-07**
   (`75a3412`): they are read off the delivered `VideoFrame`'s own index now
   rather than the decoder's per-decode perf fields, so a cache hit reports as
   honestly as a decode. Measured on the backward drag at ~92% hits, it read
   `target 5 | shown 5` with the playhead on frame 3 and now reads `target 3 |
   shown 3`. Worth remembering *why* it was only ever called a nuisance: the
   values were not wrong, they were **old**, which is harder to notice and worse
   in a line whose entire job is to say which frame is on screen.

**`TRACE_IO_READAHEAD=1`** (2026-08-15: the third LucidLink read-ahead design in `MediaIoSource`,
default off, correctness-verified but NOT validated against real remote storage — see
`docs/lucidlink-readahead-v3.md`. `TRACE_IO_READAHEAD_MB` (default 24) and
`TRACE_IO_READAHEAD_CHUNK_KB` (default 4096) tune capacity and fill granularity.
`TRACE_IO_INJECT_KBPS` (bandwidth cap) and `TRACE_IO_INJECT_DELAY_MS` (fixed per-read latency,
the model that matches what read-ahead is for) are synthetic link simulators for A/B'ing it on
local media — never a substitute for a real remote measurement. `TRACE_IO_LOG=1` appends
per-close read/seek stats including `bufferHits`/`raRebases` to `%TEMP%\trace_iolog.txt`.

**Tuning knobs**, all defaulting to shipped behaviour: `TRACE_ASYNC_SCRUB=0`
(back to the synchronous walk), `TRACE_SCRUB_WALK_MS` / `TRACE_SCRUB_REARM_MS`
(the synchronous walk's budget and re-arm, for the control A/B),
`TRACE_SCRUB_FILL_MS` (seek-walk cache fill budget during a drag, default 60ms),
**`TRACE_SCRUB_BATCH=N`** (2026-08-14: how many CONSECUTIVE frames one asynchronous scrub
request may cover, **default 4**; `0` or `1` restores one frame per request, which is the
behaviour it replaces and the in-binary negative control. It is *not* a sampling knob — every
frame is still decoded, delivered and presented individually and in order, and a §15 stride
above 1 forces it to 1. On heavy media the 8ms walk budget collapses it to 1 by itself, so
ProRes 4444 reads `batch cap 4 last 1 max 1` and is unchanged),
`TRACE_PREVIEW_DISPLAY_SIZE=0` (back to plain half-res previews),
`TRACE_REVERSE_CACHE_MB` (reverse-cache byte budget, **default 384**; the
control for any hitch measurement and the one number to change if the memory
footprint is too high),
`TRACE_SCRUB_PACE`, `TRACE_SEEK_CACHE_WINDOW`, `TRACE_AUDIO_BUFFER_MS`,
`TRACE_AUDIO_SLEW`, `TRACE_AUDIO_FIXED_LATENCY`, `TRACE_NO_AUDIO`,
`TRACE_RENDERER=cpu` (**`d3d11` is the default as of 2026-08-10** — this is the
control and the escape hatch, and an unknown value warns and falls back),
`TRACE_PLANAR_UPLOAD=0` (GATE C off,
back to swscale BGRA on the d3d11 path — the control for any planar measurement),
`TRACE_GPU_REDUCE=0` (step 9 off, back to a single bilinear tap for the
downscale — exact rather than approximate, and deliberately separate from
`TRACE_PLANAR_UPLOAD` so a planar A/B does not change two things at once),
**`TRACE_MAG_FILTER=linear`** (spec phase 15: back to a bilinear magnification
above 1:1, on **both** backends. The owner decision is nearest — a review tool at
4:1 shows pixels — and this is both the side-by-side for judging it and the way
to revert that decision without touching the rest of the phase. It is a
*separate* knob from `TRACE_NEAREST_SCALE`, which forces nearest at every ratio
including reductions and is the step 9 control),
`TRACE_DEADLINE_SCHED=0` (GATE E step 1 off, back to the fixed integer tick
and its accumulator gate — the negative control for any cadence measurement),
`TRACE_REVERSE_ASYNC=0` (reverse shuttle off, back to synchronous UI-thread
reverse — its own knob rather than sharing `TRACE_ASYNC_SCRUB`, so a reverse A/B
does not also change how dragging behaves), `TRACE_LONGGOP_SLICE_THREADS=1`
(slice-only threading for long-GOP codecs — **measured and refuted**, retained as
the control for that closed question),
**`TRACE_SCHED_FREERUN_LATE=0`** (2026-08-13, `ee6d525`: back to arming for the next
grid slot after a handler overran, instead of re-arming at once. **Default is the
re-arm**; this is the control, and it is the only knob that changes anything on a file
that misses its budget — 8K ProRes 4444 XQ reads **45.1% of real time on the default
against 35.4% with it set**, `outside 1.5 vs 29.4ms`. Inert on everything that meets
budget, and `rephase 0` is why: the branch never executes there),
**`TRACE_INTRA_FRAME_THREADS=1`** (2026-08-13, `ed686a1`: frame threading for
intra-only codecs — the **symmetric control** `TRACE_LONGGOP_SLICE_THREADS` never had.
**A large trade in both directions, not a dormant win**: 8K 4444 XQ playback
33.8 → 58.0% of real time with the handler 89.7 → 43.3ms, while 4K 4444
`scrub -SnapRelease` goes `dec 15.9 → 155.6ms` and `release 42.8 → 398ms` with
`ui gap max 26 → 241ms`, and `-Reversals` `hitch 2 → 7`. The landing stays exact on
both, so it is responsiveness rather than correctness. **It is also the control for
the per-mode threading switch that was built and removed** — see the Decisions
entry; under it the intra-only walk limit is 48 rather than 1, which is the other
half of that measurement),
**`TRACE_DECODE_THREADS=N`** (2026-08-13: the decoder's `thread_count`, default 0 =
FFmpeg's automatic count, **which caps at 16 and leaves half a 32-thread box idle**.
`=32` is worth +21% on the 8K plate — `dec 61.11 -> 45.08ms`, 44.7 -> 54.2% of real
time — and is **not** the fix, so the default is unchanged until the FFmpeg build
question is settled and a default can be set once from the build that ships),
**`TRACE_RT_DROP=0`** (2026-08-13, `35d976b`: never drop a playback frame, i.e. the
pre-owner-decision behaviour where a heavy source plays the whole movie slowly
instead of holding media time. The control for anything about the drop, and the
thing to set if a `media %` figure is ever questioned. **Note the default engages
only when a source cannot keep up** — every file in the validated set reads
`drop 0` either way, so this knob changes nothing on any of them),
~~`TRACE_SHUTTLE_ENTRY=2x`~~ (**gone as of
spec phase 5** — an interim knob added at phase 3 so the Rewind/Fast-forward
buttons' 2× entry was executable before those buttons existed; both buttons are
real now and pass `AtTwoX` as an argument, so nothing needs it), **`H` (not an env knob — the keyboard
toggle for the dev HUD, added at spec phase 2; `Return`/`Enter` still work, and
hiding it also stops the HUD line being *built*, so it is the state to judge feel
in and the wrong state to quote a bare `stalls` from)**, **`TRACE_HUD=1`** (2026-08-17,
roadmap step 2: the HUD ships HIDDEN now, and this forces it on from launch — the
measurement override, passed by `restart.ps1` by default because every recorded figure
is read off the HUD; `TRACE_HUD=0` under a harness measures the shipping look
deliberately), **`TRACE_TRANSPORT_BAR=1`** (spec
phase 6: back to the docked transport bar. The floating overlay is the default transport now,
so this is the escape hatch, the negative control for every phase-6 figure, and **what the
harness scripts that locate the timeline by scanning for its groove colour need in order to
run at all** — `scrub.ps1`, `revplay.ps1`, `lifecycle.ps1`, `transitions.ps1`,
`shuttleland.ps1`, `previewshot.ps1` and `overlay_drag.ps1`'s control leg. `TRACE_OVERLAY=0`
and `TRACE_OVERLAY_COMPOSITED=0` select it too, so turning the overlay off asks for the other
transport rather than for none), `TRACE_OVERLAY=1` (the floating transport — **on by default
since spec phase 6**; `TRACE_OVERLAY_COMPOSITED=1` is retained because the harness sets it),
**`TRACE_SHAPE_LOG=1`** (spec phase 12: one stderr line per window-shaping pass, printing every
term of the calculation **and what the layout actually did with it** — the two disagreeing is
the failure mode there and is invisible from `win WxH`. Through `fprintf`, not `qWarning`: in
this GUI-subsystem build Qt's handler does not reliably reach a console's stderr, and the first
version printed nothing while FFmpeg's own messages came through the same run),
**`TRACE_ASYNC_LANDING=0`** (2026-08-14, `cc8e638`: back to decoding the exact landing --
a groove click, a slider release, a frame step -- on the UI thread. **Default is the worker**;
this is the negative control for every landing figure and the rollback. It changes WHERE the
decode runs and nothing else: still `RequestMode::Step`, one frame, full resolution, accurate
conversion. It also restores per-press stepping, since the async path coalesces rapid presses),
**`TRACE_PLAYBACK_QUEUE=N`** and **`TRACE_PLAYBACK_QUEUE_MB`** (2026-08-14, `d8beba8`,
checkpoint 2 stage one: how many frames ahead ordinary 1x forward playback may decode on the
worker, and the byte budget bounding it. **DEFAULT 0 = OFF**, so nothing ships enabled and the
synchronous path is the comparison. **Depth 1 is worse than off** -- a depth-1 queue cannot
overlap -- and **depth 2 is the minimum that overlaps**; the byte budget clamps 8K to 2 by
itself. Worth ~+10% on the 8K plate and nothing on a file that already meets budget, where it
simply moves the decode off the UI thread),
~~`TRACE_STRIP_BACKDROP`~~ (**gone as of 2026-08-19** -- the painted strip blur it gated was
removed with owner item 8's resting translucency, because the alpha sweep measured
blur-under-a-resting-alpha as the worst combination for label legibility. The
`EnableTransparency` tri-state read it introduced SURVIVES, moved into `TopChrome` and gating
the resting alpha instead: Windows transparency off means the strip rests opaque, exactly the
package's own solid `#14161A` fallback case, and `WM_SETTINGCHANGE` still applies it live.
**Read the HUD's `strip` field rather than the command line** -- the answer is still not
decided by the launch, and on cpu the layered alpha is silently ignored, so a capture must
carry what the strip believes it is doing),
**`TRACE_TOPCHROME_FADE=0`** (2026-08-18, owner item 11; widened 2026-08-19 by item 8: back to
the top strip popping on and off, OPAQUE AT REST, instead of fading by layered-window alpha to
a resting alpha of 215. **Default is the fade-to-resting-translucency**; this is the full
rollback for both and it never applies `WS_EX_LAYERED` at all. The fade and the resting
translucency are real only on the d3d11 default — cpu ignores the alpha for a known
backing-store reason and keeps the opaque pop either way, an owner-accepted divergence — and
**both depend on `app/trace.manifest`'s Windows 8+ supportedOS declaration**, without which
the alpha is silently ignored on both. `TRACE_TOPCHROME_ALPHA=N` (0..255) pins the alpha so a
mid-fade state can be captured as a stable state; `=255` doubles as the opaque-resting
override. Harness: `scripts/measure/topchromefade.ps1` — its `rest` mode checks the shipping
resting blend per renderer, with cpu's PASS being "opaque"),
**`TRACE_THEME_LOG=1`** (2026-08-18, roadmap step 10: print the font family that ACTUALLY
resolved and the `Segoe UI Variable` families **Qt** can see, which are not the ones GDI lists.
It exists because the first build of `src/app/Theme.*` asked for a family Qt does not enumerate,
`hasFamily()` declined it, and the application ran on Segoe UI looking very nearly right. The
same value is on the dev HUD as `font`),
**`TRACE_SETTINGS_FILE`** and **`TRACE_SETTINGS_LOG=1`** (spec phase 11: point the settings
home at a scratch INI, and print which home won. The first exists so a measurement of the
recent list does not edit the machine it runs on and can start from a known list; the second
because which of the three homes is in force is a *path*, and a path is the thing a 15px HUD
capture is worst at. **`TRACE_SETTINGS_FILE` is no longer only the recent list's**: spec phase
14 added Loop, which is persisted, and a wrap re-establishes the playback timeline and zeroes
every cadence counter with it — so `cadence.ps1` passes a scratch INI too, and a cadence figure
taken on a machine where Loop was left on reports the **last lap** while reading a healthy
100%. Measured: `frames 30 | elapsed 1.25s` against a 119-frame baseline),
and ~~`TRACE_VIEW_TRANSFORM`~~ (**gone as of spec phase 10** -- the interim rotate/flip knob; the Edit menu's five actions replaced it and it left with the phase that made it redundant, exactly as `TRACE_SHUTTLE_ENTRY` did at phase 5), **`TRACE_LUCID_LOG=1`** (spec phase 9: one stderr line per LucidLink probe and copy -- the gate is three refusals deep and `disabled` looks identical whichever one fired) and **`TRACE_LUCID_COINIT=1`** (the retained control for the apartment question: `CoInitializeEx` instead of `OleInitialize`, measured identical).

**Experimental / diagnostic gates, all off unless set** — confirmed at runtime,
a default launch reports `renderer d3d11 +overlay` (`renderer cpu` before 2026-08-10; **the
floating transport is drawn by default since spec phase 6**, and only bar mode announces
itself on stderr now) and writes no other Trace diagnostics:
`TRACE_OVERLAY_COMPOSITED=1` (the spike's original name for the overlay, retained because the
harness sets it — it no longer selects anything a default launch does not already do),
`TRACE_OVERLAY_SPIKE=1|2|3` (the superseded Qt-widget overlay probe that proved
the widget route closed), `TRACE_D3D11_CLEAR_DIAG=1` (clears the back buffer red
instead of black -- the diagnostic that separates "not presenting" from
"presenting black", and the one that located the GATE B fault),
`TRACE_D3D11_HOSTHWND=1` (presents into the host HWND instead of the child
window; kept for the A/B only, **not** a supported configuration).

**The HUD line to reach for on a "feels wrong" report is now `ui`, not
`smooth`.** `ui gap` is measured by a 1ms timer that can only fire when the
event loop is running, so its worst interval is how long the window could not
deliver a mouse move or repaint -- the thread, measured from outside the work it
is doing. `smooth` says when frames landed; `ui` says whether the app was alive.
They answer different halves of "stable but not smooth" and the slider-yank bug
above is what happens when you only have one of them. Note `uiblock seek` on the
`resp` line measures the *worker* while a lease is out, not the UI.

**Scaling quality has a harness now**: `scripts/measure/abfilter.ps1` places a
capture on an axis between two ffmpeg references at the exact drawn size —
`area` (0) and `neighbor` (1) — scored by mean |Laplacian|, because
high-frequency energy is what separates aliasing from mere difference. It is
**calibrated** (area 0.00, bicubic −0.01, lanczos 0.06, bilinear −0.20,
fast_bilinear 0.74, neighbor 1.00) and `-Sensitivity` **refuses material whose
two references agree** — it rejected the 4K milk splash and the 60fps drone
plate, either of which would have passed silently. Use 4444 or 422 HQ.
`croprect.ps1` cuts the video rect out of a window capture and asserts its size
against the HUD's `display`, because a one-pixel crop error on a 6x reduction
reads as a filtering difference. `previewshot.ps1` captures with the mouse button
still **down**, since the release is what lands a full-resolution frame.
**Never use Trace as its own reference here** — §20.3 spent a session on a
CPU-vs-GPU difference where both sides were the same 2x2 tap.

**~~The step 10 strip backdrop's two harnesses~~ RETIRED WITH THE BLUR, 2026-08-19** —
`stripbackdrop.ps1` and `backdropcost.ps1` measured a mechanism that no longer exists and are
deleted; `topchromefade.ps1 -Mode rest` is the resting-strip check now. Two things they
established are carried rather than lost: the `hsd`-is-exactly-0 discriminator reasoning (a
purely vertical gradient has zero horizontal variation, a copy of video cannot — note that on
d3d11 the RESTING strip now blends real video, so its band `hsd` is nonzero BY DESIGN and
`hsd 0` at rest would now mean the translucency is broken or cpu), and the two auto-hide facts
in the next paragraph, which every chrome harness depends on.

**TWO HARNESS FACTS THEY ESTABLISHED, BOTH ABOUT THE AUTO-HIDE.** **A stationary pointer anywhere
INSIDE the client holds the chrome up indefinitely** -- probed at three positions on a paused
clip, the centre and the bottom corner are still up at 6s unchanged to three decimals while
outside the window it hides at ~2.6s, because the timeout handler holds while `hover_` is a
region. So a leg that needs the chrome hidden must park the pointer **outside**, as `overlay.ps1`
always has; and a run that needs it **up** should park it over the picture, which holds it for the
whole run and generates no input at all -- strictly better than jiggling, which the earlier
measurement had to do on both sides. And **`keyPressEvent` calls `revealOverlay()`**, so any leg
that changes the frame with the arrow keys is holding the chrome up with the same gesture: the
first version of the `revive` leg tested nothing for exactly that reason and passed on a build
with the fix removed.

**`themeshot.ps1` opens each of the five menus and asserts the capture changed** (2026-08-18,
roadmap step 10). The popup menu is **the one surface the design package does not show** -- all
three of its screens have the menus closed -- so its colours, radius and spacing are derived
rather than copied, which makes looking at it the check. It captures a region **around** the
window because a menu is a separate popup window, and it opens them **by mnemonic, never by
counting DOWN arrows**, both of which `menushot.ps1` already records.

**Open Recent has a harness now**: `scripts/measure/recentfiles.ps1` (spec phase 11), seven
modes. **Run `-Mode calibrate` beside any startup result you quote** — it prints what a stat on
the seeded unreachable paths actually costs (21,037ms), which is the only thing that makes
"startup did not move" a measurement rather than an assertion. `-Mode startup` seeds ten
entries including two unreachable UNC hosts and times launch-to-window against an empty list;
`-Mode missing` (with `-Keep` as its negative control) drives the missing-file prompt;
`-Mode behaviour` covers MRU, de-duplication and the present-but-undecodable case;
`-Mode home` runs all three settings-home branches. It uses `TRACE_SETTINGS_FILE`, so it never
writes the real per-user file. **`scripts/measure/swapexe.ps1`** does the control-binary swap
every phase since 6 has done by hand and prints the hash of what is actually live.

**The Movie Inspector has a harness now** (spec phase 13): `scripts/measure/inspector.ps1`,
four modes. `-Mode show` opens media and `Ctrl+I` and captures both windows; `-Mode viewport`
resizes and checks the observed row follows — **it steps a frame afterwards to force a HUD
refresh**, without which `display` is stale and the two numbers being compared are from
different moments; `-Mode media` opens a second file through File ▸ Open **in the same
process**, because a second launch tests nothing about "update when active media changes"; and
**`-Mode hold` is the only leg that can fail on a plausible build** — the negative control on
the modeless window not holding the floating transport revealed. Read its `guard` line first:
`hidden → revealed` must be ~4.2% or nothing below it means anything. Its second leg reports
**NOT RUN** rather than a number, because Windows refuses `SetForegroundWindow` to a background
process once the inspector has taken focus.

**Menus, help and accessibility have harnesses now** (spec phase 14).
**`scripts/measure/uiatree.ps1`** walks the UI Automation tree a screen reader reads and prints
every element's name, control type, rectangle and description — **run it under
`TRACE_TRANSPORT_BAR=1` as the negative control**, which is what showed the docked bar
announcing five unnamed `Group`s. An element absent from that tree is certainly not announced;
an element present in it can still read badly aloud, so **it does not close §31.5 item 4**.
**`phase14.ps1`** covers `speed` / `loop` / `copy` / `close` / `shortcuts`; its `loop` leg needs
a scratch `TRACE_SETTINGS_FILE` or the control is the test run twice, since Loop is persisted.
**`menushot.ps1`** opens each menu and **asserts the capture changed** — a menu is a separate
popup window, so a `GetWindowRect` capture of the main window shows a closed menu bar whether or
not it opened. All three navigate menus **by mnemonic, never by counting DOWN arrows**: counting
has to know the first item is already highlighted and skip separators, and a miscount silently
activates the item *next* to the one under test.

**View scaling has a harness now** (spec phase 15): `scripts/measure/viewscale.ps1`, four modes.
`-Mode ladder` presses Ctrl+= and Ctrl+- six times each and **stitches the one HUD line that
matters into a single strip**, because a ladder is only meaningful as a sequence and reading
`zoom N:1` off thirteen full-window screenshots is thirteen separate readings. `-Mode actual`
asserts `display WxH` **equals the source's own pixel size**, which no screenshot can tell you
by eye. `-Mode filter` reads the magnification filter back (`NEAREST` against `filtered xN`).
**`-Mode pan` is the only leg with a negative control and it needs one**: it drags twice — once
at fit where the picture must NOT move, once at Actual Size where it must — so a build with no
pan at all reads ~0% on both and passes nothing. **Every mode prints `win WxH` before and
after**, because the owner decision is that Actual Size pans inside the viewport and leaves the
window alone.

**Window shape has a harness now** (spec phase 12): `scripts/measure/resizecache.ps1` drives a
real corner drag and reports `resize/chg/drop/sync` and the three Win32 resize messages —
**read `drop`, the entries discarded, not the number of clears**, because clearing an empty
cache is free and a count of clears reads as a 122x thrash that is not there; its nearly-empty
leg is the control that says the fill worked at all. `scripts/measure/make_shape_fixtures.ps1`
generates the anamorphic and rotation-metadata material the asset set does not contain, and
**`rotated-180` is the fixture that matters** — the only one that fails a build which transposes
on any nonzero rotation. `TRACE_SHAPE_LOG=1` prints the sizing calculation and what the layout
did with it.

**Mixed-monitor DPI has a harness now** (2026-08-14, plan §20.4): `scripts/measure/dpimove.ps1`,
seven legs — `enum` / `move` / `maximized` / `fullscreen` / `open` / `cadence` / `resize`.
**It is NOT a CI step**: it needs two physical displays at different scale factors, which is
exactly why §20.4 sat tabled for five days. Four things it does that the others do not, each of
which cost a run to learn. **It sets `PER_MONITOR_AWARE_V2` and refuses to measure without it**
— `powershell.exe` is *system*-DPI aware, so every other script here virtualizes rects and
captures for a window on the 150% monitor. **Every leg steps a frame after moving**, because
`refreshHud` is not called on `resizeEvent` and a paused window redraws the HUD at the new size
with the old string in it. **`-On primary` is the control** on the single-monitor legs, and it
earned itself immediately by attributing a 7px fullscreen-restore offset to the oversized dev
HUD rather than to DPI. **`-HideHud` is the shipping configuration** and is what separates a
product defect from phase 12's diagnostic limitation. `-Mode open` passes the path through the
**clipboard**, never SendKeys, which mangles every `( ) + ^ % ~ { }` in it.

**Lifecycle gestures have a harness now**: `scripts/measure/lifecycle.ps1`
covers step +/-5 determinism after a release, play-after-release, opening
another file mid-drag, and quitting mid-drag. Those are the transitions where an
ownership bug shows up as a hang, a stale frame or a wrong landing rather than
as a bad number, and no throughput harness reaches them.
`scrub.ps1 -SnapRelease` releases with no settling pause -- the only gesture
that reliably catches a decode in flight, and therefore the only one that
exercises cancellation at all.

**Reverse playback has a harness now**: `scripts/measure/revplay.ps1` (2026-08-10).
It clicks to position the playhead (a click jumps and lands exactly; a drag would
shuttle every frame there and pre-fill the cache the run is about to read),
presses J `-Presses` times for -1x/-2x/-4x, holds, and **captures before K** --
the cumulative counters survive the stop but `speed` does not. `-StepCheck` is the
landing-exactness gesture and **both its legs must be read**: the `-1` leg is the
result, the `+1` leg is the control proving the comparison can see a moved picture
(4K H.264 reads `+1 moved 7.5%, -1 returned 0%`). Reverse is silent, so no
`TRACE_NO_AUDIO` control is needed -- every file is already on the same scheduler.

**A bare click and a bare step have harnesses now** (2026-08-14): `clickland.ps1` times one
groove click as the longest stretch during which the window stops answering messages, and
`stepcost.ps1` does the same per step for N steps. Neither is reachable from a drag harness —
`scrub.ps1` presses and immediately sweeps, so the press cost is folded into the gesture, and
stepping never involves the scrub worker at all. **`widen.ps1` is what lets any of the groove-
scanning harnesses run on portrait media**: at the §4 width a 9:16 window's slider is under
`scrub.ps1`'s 300px minimum run and it reports `groove not found`. Widening is **free** on
portrait media because the fit is height-bound, and the script prints `display` either side so
the run carries its own proof; it steps +1/-1 afterwards because `refreshHud()` is not called on
`resizeEvent`.

**Quote `hitch`, not `stalls`, and quote `win WxH` with either.** `stalls` is
`gap > 2 x refresh` and this box has been observed at both 239.999Hz and 60Hz,
so the same run reads 51 or 3 (plan §26.1). `hitch` is a fixed 33ms bar and is
the only one comparable across sessions. Cache depth is also a function of
window size and dominates (§22.8).

**And quote `display` too, because roadmap step 7 made the window ALL picture.** The chrome
term is `0x0` since `10a7fba`, so the video rect is the client area and every scrub figure
moves with it. The §4 default window **with the HUD forced on is bistable** — the HUD's height
depends on the window's *width*, so the two-pass convergence can settle either way, and it was
observed at `win 728x795` one session and `win 1278x1083` the next **on binaries built from the
same commit**. The shipping HUD-hidden size is not affected. Never compare a HUD-on geometry
across sessions.

**Measurement note:** the HUD is unreadable in a normal screenshot on the
5120x1440 panel -- it downsamples too far. Capture the Trace window at native
resolution instead (GetWindowRect + Graphics.CopyFromScreen). Synthetic drags
that teleport the pointer and pause overstate how well the shuttle keeps up;
use a continuous sweep with a spin-wait for realistic pacing. A single smooth sweep is NOT enough to find correctness bugs: the decode-error in `2523d77` only appeared under hard direction reversals and runs into both ends of the clip, held under one continuous button press. Keep both gesture sets. Find the timeline
groove by scanning for the RGB(55,55,55) track rather than assuming a y offset:
the transport sits above a HUD whose height depends on the media.
**Two traps in that scan, both hit in Aug 2026:** take the *longest* run in the
transport band rather than the first match anywhere, or window chrome wins; and
RGB(55,55,55) is the **unfilled** track, so it does not exist when the playhead
is at the end — restart the app (which opens at frame 0) before locating the
groove rather than trying to find it mid-clip.

## Working conventions

- **The `V:\` LucidLink mount on the test box is live client production storage and is STRICTLY READ-ONLY.** Never create, copy, move, rename, delete or modify anything on it, and never stage test media there. Read only files Anj nominates; do not browse project folders. Storage-detection code must identify the volume by querying it (`GetDriveType`, capacity/free), never by writing a probe file.

- Commit style follows the existing log: `playback:`, `perf:`, `ci:`, `docs:`, `fix(windows):` prefixes with imperative subjects.
- Keep changes conservative and testable per push — Anj can only validate via CI ZIP builds on Windows, so each push should be a coherent, revertable step.
- Update this file's Roadmap/Decisions sections at the end of each working session so the next session starts current.

### Traps this project has paid for more than once

- **`git add` succeeding is NOT evidence a file was added.** `.gitignore`'s build-tree patterns
  are anchored (`/build/`, `/build-*/`) precisely because an unanchored `build-*/` matched
  `scripts/build-ffmpeg/` at depth and swallowed a whole directory: `git add -A scripts` reported
  success, pushed nothing, and CI failed with "the term … is not recognized". **Run
  `git ls-files <path>` after adding anything under a new directory.**
- **PowerShell 5.1 promotes a native command's stderr to a TERMINATING error under
  `$ErrorActionPreference = 'Stop'`, even when the process exited 0.** A build that merely warns
  looks exactly like a build that failed — it aborted three separate steps in one session, once
  on a completed FFmpeg build. Demote to `'Continue'` around native calls and read
  `$LASTEXITCODE`, or redirect inside bash rather than in the PowerShell pipeline.
- **The FFmpeg build needs msys2's `make`, never `mingw32-make`.** The native make spawns each
  recipe through `cmd.exe` and inherits its ~32K command-line limit, and FFmpeg hands `makedef`
  every object file of a library as one argv. It fails as
  `Object does not exist: libavcodec/h261_parser.` — a filename truncated mid-word, which reads
  like a corrupt source tree and is not.
- **Run `dumpbin /dependents` over EVERY produced DLL, not one of them.** A dependency that
  resolves from your own toolchain directory is not a dependency you have satisfied:
  `avutil-60.dll` imported `libwinpthread-1.dll`, passed locally because gcc's bin was on PATH,
  and failed CI with exit **-1073741511** (`0xC0000139`, STATUS_ENTRYPOINT_NOT_FOUND). To
  reproduce that class locally, launch with `PATH` reduced to `System32`.
- **A stride-unaware `LockBits` pixel diff walks row padding and invents differences.** It
  reported 399 differing pixels at max delta 171 on four unrelated files — **identical figures
  across unrelated inputs is the tell.** Compare only the first `width*4` bytes of each row.
- **XML forbids a double hyphen inside a comment, and that now covers the SVG masters as well
  as `app/resources.qrc`.** resvg enforces it where some renderers do not, so a comment written
  in this project's usual voice fails the rasteriser rather than the build.
- **To rasterise an SVG on this box, `pip install resvg-py`.** There is no `magick`, `inkscape`
  or `rsvg-convert`; `cairosvg` installs and then cannot load a cairo DLL, because the pycairo
  wheel links cairo statically into its own `.pyd` and leaves nothing for `cairocffi` to open.
  `resvg_py.svg_to_bytes(svg_path=..., width=N, height=N)` returns PNG bytes and handles
  gradients and `mix-blend-mode`.
- **A control binary copied out of `build/app/Release` will not start.** It has no Qt DLLs
  beside it, and the failure arrives as a modal "Qt6Widgets.dll was not found" box that then
  sits on top of the next capture. Check out the parent commit and rebuild in the same build
  directory instead, and verify the swap by hash.
- **Benchmark the libraries you ship, at the settings you ship, and subtract what your harness
  serializes that the reference overlaps.** Two 8K figures in this repo were wrong for the first
  reason and one for the second.
