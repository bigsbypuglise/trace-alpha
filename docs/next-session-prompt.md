# v0.2.0-beta.1 IS SHIPPED. THE OPEN PHASE IS THE OWNER'S UI REDESIGN ROADMAP.

## 2026-08-18, THIRD SESSION: ROADMAP STEP 5 IS DONE — THE EDGE-TO-EDGE TRANSPORT STRIP

Four commits: `15473d0` the strip and its glyphs, `02b07e5` the harness, `cdc0959` the docs,
and the Loop button after the owner asked for it. The floating 460×84
panel is gone; the transport is the design package's **56px strip across the full width of the
window** with nine controls — `|◀ ◀◀ ▶ ▶▶ ▶|`, mute, the timeline between its two readouts,
fullscreen, a separator, share.

**Read the step 5 block in `CLAUDE.md` before quoting any figure.** This session ran on the
**physical panel, 5120x1440 @ 239.999Hz**, so nothing in it is comparable to step 7's
1920x1080 record; a control was built from `637c7e5` and hash-verified (`578A6C03` against
`0806244E`) and run beside every leg.

**Six things to carry.**

- **THE TIMELINE IS DRAWN, NOT REWRITTEN.** `timelineSlider_` is still the whole scrub state
  machine and the track is a picture of it — a press still runs `setSliderDown` / `setValue` /
  `setSliderDown` through `OverlayHooks`. Nothing in the strip computes a target, which is why
  `-SnapRelease` still reads `target 261 shown 261 delta 0` full-res planar, identical to the
  control. **If a future step touches the track, this is the property to re-confirm first.**
- **THE PHASE 6 GEOMETRY IS SUPERSEDED, KNOWINGLY** — 460×84 / 44 / 34 was signed off with "no
  tuning is wanted" and the owner replaced it with 56 / 40 / 36. **`kFadeMs` and `kAutoHideMs`
  are NOT superseded.** Both the phase 6 sign-off and the `overlay.ps1` note in `CLAUDE.md`
  now say so at their own site.
- **THE ROADMAP'S OWN PREMISE FOR THE MUTE GLYPH WAS WRONG, AND THE START/END ART DOES NOT
  EXIST EITHER.** The package carries **one** `volume`, in `source/` only, and no muted
  variant; and the retired 260807 set's `prev-clip` is a **double triangle**, i.e. the same
  shape as `rewind`. Three masters were authored from the delivered ones (same viewBox, fill
  and 1.6 stroke; go-to-end an exact mirror about x=12), `volume` was promoted out of
  `source/`, and there is **deliberately no `volume-low`**. Derived set **29 → 37** with no
  edit to `verify_trace_assets.py`.
- **EVERY CELL IS RASTERISED AT THE SIZE IT IS DRAWN, AND THE TWO DEPARTURES WERE MEASURED.**
  Cross-backend over the empty state's black stage: stretched gradient column **12,511 px at
  delta 3** → 1:1 cell with draw-time alphas **3,594 at delta 2** → 1:1 cell with the design's
  alphas **baked into their own cells** **0 px, delta 0**. A fractional alpha applied by
  QPainter's integer SourceOver on one path and a float multiply in the shader on the other is
  a real difference; baking it leaves only the fade, which is exactly 1.0 when revealed.
- **A CROSS-BACKEND DIFF TAKEN OVER VIDEO CANNOT SEE THE STRIP.** The strip band over a paused
  frame reads 44.9% at delta 3; the *same band with no strip* reads 43.9% at delta 4 and the
  bare video band 65.3% at delta 20 — the translucent strip attenuates the video's own backend
  difference. **Compare the strip over the empty state**, which `emptystate.ps1 -Mode
  transport` already sets up.
- **`QAction::toggle()` EMITS `toggled()`, NOT `triggered()`.** The Mute button flipped the
  tick and never reached the audio for one build. It was found by a harness assertion **only
  after that assertion was corrected** — the first version compared against a capture taken
  before the pointer arrived, so the hover plate changed all 625 pixels and it reported PASS on
  the broken build.

**Three owner decisions closed the step and all three are SETTLED, not defaults.** **Loop is
built** — the tenth control, after Mute, and **the one whose state is not a second glyph**: the
package ships one loop glyph, so ON is the accent and OFF is neutral, and its `CheckBox` proxy
role is the only thing carrying that state to a screen reader. **The three authored glyphs
stay** ("for now"), which makes them shipped artwork rather than placeholders — still a drop-in
swap if a designer draws replacements. And **disabled controls stay visually unchanged**, which
was already true of every control predating the step. **Do not "fix" the last one.**

**Regression flat against the control**: cadence 4K H.264 ×2 **99.1/99.1%** with identical
buckets, `drop 0`, `rephase 0`, `0 of 120` and the same paints · `-SnapRelease` `delta 0`,
`hitch 0`, `land 0`, release 21.4 vs 21.1ms · reversal drag **`hitch 1` on six of seven runs
against 1 on four of four** · lifecycle **83.9% / 0%** against **82.4% / 0%** · **25 of 25
transitions on both** · `emptystate.ps1` all four modes on both backends plus the `-Bar`
control · `uiatree.ps1` ten named controls on the drawn rects.

**Two harness changes, both because a detector met chrome it was not written for — the third
and fourth instances.** `emptystate.ps1`'s "essentially black" bound was under 12 and the
strip's translucent gradient composites to 6..10 over black, so the bound swallowed the strip
and the mark scan swept its **accent** — the transport's first strongly chromatic thing — into
the mark's box, reporting a 433x377 mark against the design's 59x68 on a correct build. The
stage measures exactly 0.00 and the strip 6.00..10.77; the threshold is **3**. And
`overlay.ps1` asserted a 460×84 panel with control positions as a *fraction of the panel
width*; it now asserts the design's 56px height, checks the width against the captured window,
and derives every control as a multiple of the measured strip **height**, with four new legs
for the four new controls.

**Steps 6 and 8–12 carry their flags unchanged; the next one is the owner's to choose.** Step
10's Mica/Acrylic backdrop is still the item with real presentation risk against a flip-model
swapchain, and the strip is where it would land.

---

## 2026-08-18, SECOND SESSION: ROADMAP STEP 7 IS DONE — THE TRANSIENT TOP CHROME

One commit (`10a7fba`), and it **finishes step 4**: the menu bar was the last permanent chrome,
so `windowChromeLogical()` now reports **`chrome 0x0`** and the client area is entirely picture.
`src/ui/TopChrome.*` is a strip that floats over the top of the video, holds the **real
`QMenuBar`**, and is shown and hidden by the **same reveal state** `OverlayModel` keeps for the
transport — one idle timer, one hold list, one fade state, through
`OverlayHooks::setChromeRevealed`. Gated on **`barIsDocked_`**, so `TRACE_TRANSPORT_BAR=1` keeps
the old menu bar and the old geometry and the groove-scanning harnesses are unaffected.

**Read the step 7 block in `CLAUDE.md` before quoting any figure. It is the standing reference
and the 2026-08-17 one is not** — every §4 opening size moved again. Display was 1920x1080 @
59.999Hz; a control was built from `d9d4d98` and hash-verified (`1DCAFEB7` vs `1F4F39E9`) and run
beside every leg.

**Five things to carry.**

- **A NATIVE SIBLING OF THE D3D11 SURFACE WINDOW CORRUPTS THAT SURFACE'S OWN OVERLAY PASS.** As
  a native child of the *viewer*, the strip is visible and hit-testable exactly as plan §18.4
  predicts — and the transport panel's **first quad draws correctly while every quad after it
  renders as if its sampled colour were zero**, on the default renderer only. **It is not a data
  fault**: the atlas texture was read back from the GPU into a staging copy and is byte-identical
  to its `QImage`, including an opaque white texel at a pixel that drew black, and the uvs, alpha
  and brighten are all correct per quad. Parented one level up, to the central widget, it is gone
  completely. **The distinction is where in the HWND tree the native window sits relative to the
  swapchain's own, not whether it is native.** Cause deliberately unattributed. Found by
  bisecting against a control build.
- **A HIDDEN `QMenuBar` LOSES ITS MNEMONICS, BY QT'S OWN MECHANISM.** `grabShortcut` shortcuts
  are declined for an invisible widget, so `Alt+F` would silently do nothing. An event filter on
  the **window handle** reveals the chrome first — it has to be there, because key events reach
  the shortcut map inside `QWidgetWindow::event`, before any widget sees them. Verified from a
  hidden strip: all five mnemonics open their menu, and a mouse click on the strip does too.
- **THE STRIP IS OPAQUE AND THAT IS THE DESIGN'S OWN FALLBACK.** §18.4 measured that every
  native-surface variant loses translucency. The package supplies `#14161A` for exactly this
  case, and the backdrop blur it falls back from is roadmap step 10. Native on **both** backends
  deliberately, so no cross-backend comparison contains a real difference; the strip's own rows
  are identical to the pixel.
- **THE EMPTY STATE IS NOW CENTRED IN THE STAGE THE CHROME LEAVES** (`OverlayModel::setTopInset`,
  read by the empty state and nothing else). Optical y offset **−38.0 → −19.0**, against bar
  mode's −20.0, identical on both backends — which is what the design's own markup does and what
  the window did while the menu bar was in the layout.
- **TWO HARNESSES WERE DEFEATED BY CHROME THEY WERE NOT WRITTEN FOR, AND BOTH ARE FIXED.**
  `overlay.ps1` locates the panel by difference and the strip now reveals with it, so the box
  spanned both and read 1253x675; it skips the strip's band. `emptystate.ps1`'s mark scan was
  unbounded at the bottom because "the chrome down there is neutral" — **false in bar mode**: the
  status bar's `Ready` is subpixel-antialiased at a **median chroma of 115 against the prism
  mark's own 58**, so no threshold separates them. It has a measured bottom bound now.

**Fullscreen gets the SAME strip** and works there. The design's screen-2 shows a different one
— 52px, filename bold beside a dimmer `1920x1080 - 24 fps`, no menu bar — which is a second
layout plus new content, so it is an owner decision rather than an oversight. One strip keeps
the menus reachable in fullscreen.

**The §4 default window WITH THE HUD FORCED ON is bistable** — the HUD's height depends on the
window's width, so the two-pass convergence can settle either way (last session `win 728x795`,
this one `win 1278x1083`, on **both** binaries). The shipping HUD-hidden size is not affected.
Quote `win` and `display` from the run itself.

~~**Steps 5, 6 and the rest carry their flags unchanged**~~ — **step 5 is DONE, see the top of
this file**; its two flags were answered by the owner and then built. Step 7's accessibility
flag is closed by the shape that was built.

---

## 2026-08-18: ROADMAP STEP 3 IS DONE — THE POLISHED EMPTY STATE

One commit (`72aa9ac`). The prism mark and its hint line are a **fourth image in
`OverlayModel`**, beside the atlas, the rate text and the toast, emitted **outside both
gates** — outside the opacity gate like the toast so it cannot fade, and outside `enabled_`
so it survives `TRACE_TRANSPORT_BAR=1`. **It removes a duplication rather than adding one**:
the literal and its drawing code existed independently in `CpuImageRenderer` and
`D3D11VideoRenderer`, and not even as the same mechanism. `setPlaceholderText` and
`ViewerWidget::setCenterText` left with it; the latter had no callers at all.

**Read the session block in `CLAUDE.md` before quoting any figure.** Display was 1920x1080 @
59.999Hz, so nothing here is comparable to a physical-panel record.

Against the design package's own mockup, on both backends: mark ink **59x68** (mockup 59x68),
optical offset **+10.5px** (mockup +9.5), hint **157x13** centred **+0.5px**, gap **45px**
(mockup 45). **Cross-backend empty window: 0 differing pixels, max channel delta 0.**
Regression against a control built from `893237c` and hash-verified: cadence ×3
**99.1/99.1/99.2%** with buckets and percentiles identical to the control · `-SnapRelease`
`delta 0` / `hitch 0` / `land 0` · both lifecycle legs **40.7% / 0%, identical to the control
to the digit** · **25 of 25 transitions** · `overlay.ps1` cross-backend `08-mid-drag`
**0 px, max delta 0**.

**Four things to carry.**

- **A RECORDED FIGURE IS A RECORD, NOT A BASELINE, UNLESS A CONTROL WAS TAKEN BESIDE IT.**
  Last session's re-baseline has 4K H.264 cadence at **100.0%**, `win 728x795`. Today the same
  clip through the same harness reads **99.1%** at `win 703x794` — **on a binary built from
  that same commit as well as on the new one**, with captures the same size to the pixel. The
  machine moved, not the code, and building the control is the only reason that is known.
- **THE PRISM ANIMATION IS NOT BUILT AND ITS PREMISE NEEDED CORRECTING.** The roadmap called
  it "a small gradient animator", which is true only if the mark is drawn as QPainter paths
  and gradients **in code** — a different shipping decision from the committed PNG renditions,
  not a layer on top. Doing both gives one mark two sources of truth and leaves the embedded
  PNGs unused. **This is an owner decision.** If it is ever built, `rebuildEmpty`'s
  `mediaPresent_` branch is already the one place that decides both "the empty state is
  showing" and "the animation is running".
- **`empty-mark.svg` IS NOT `trace-play-mark.svg`.** The empty-state mark and the app-icon
  mark differ in five values and the wrong one looks almost right. Two values in the master
  are animation *phase* rather than design, set so t=0 is the frame the delivered mockup
  shows — the package's own still and its own t=0 do not match.
- **`scripts/measure/emptystate.ps1` is new and no other harness can reach this state**, since
  `restart.ps1` takes a mandatory `-Clip`. Modes `launch` / `transport` / `close` / `swap`;
  `-Bar` is the negative control for the "outside `enabled_`" claim. Its detector was proven
  able to fire before its negative result was believed.

~~**Steps 5, 6, 7 and the rest carry their flags unchanged**~~ — step 7 is DONE, see the top of
this file. Step 5's prev/next and volume flags still need an owner answer before any work starts.

---

## 2026-08-17, SECOND SESSION: ROADMAP STEPS 2 AND 4 ARE DONE, RE-BASELINED

Three commits (`5ff6431` · `635656a` · `c63aba4`) and one re-baseline, exactly the shape the
roadmap's cross-cutting section asked for. The shipping window was **menu bar + picture** after
this pass and is **picture alone** since step 7: the
dev HUD ships hidden (`H` toggles, `TRACE_HUD=1` forces it on — `restart.ps1` passes that by
default so every harness keeps working) and the status bar is never constructed in overlay
mode. All 35 `statusBar()->showMessage` sites route through **`showTransientMessage`** — bar
mode keeps the status bar, overlay mode draws a **composited toast** on both renderers that
survives the transport's fade. The menu bar was untouched here and moved at step 7 (above).

**Read the session block in `CLAUDE.md` before quoting any figure** — the full re-baseline
(cadence ×7, `-SnapRelease`, reversal drag, both lifecycle legs, 25/25 transitions, §4
geometry per shape) is recorded there, taken on **1920x1080 @ 59.999Hz**, and every §4
opening size moved with the chrome. **But see the 2026-08-18 block above: its 4K H.264
cadence figure did not reproduce on a control built from its own commit, so treat that table
as a record rather than as a comparison baseline.** Three operational traps it records: 16:9
media now needs `widen.ps1` after `restart.ps1` before any groove-scanning harness (the §4
window with HUD chrome at this display is 656px wide, under the 300px groove minimum);
`transitions.ps1` runs with `-Env TRACE_TRANSPORT_BAR=1,TRACE_HUD=0`; and the D3D11 overlay
quad loop's *shape* is load-bearing — a pointer-to-ComPtr switch rewrite drew nothing while
reading as equivalent, found only by bisecting against a control build.

---

## 2026-08-17, FIRST SESSION: THE 260817 UI v2 ASSET SWAP IS DONE — THE REDESIGN IS NOT STARTED

The owner's roadmap arrived and is annotated at **`docs/ui-redesign-roadmap.md`** — read it
before doing any UI work; the flags name where a step re-opens a signed-off decision or moves
a recorded baseline. This session did **only the asset swap**, in three commits: the delivered
package committed untouched at `assets/source/260817-trace-ui-v2/` (renamed from a folder name
containing a backtick — PowerShell's escape character); the ten glyphs and app-icon PNG/SVG
sets swapped in place with **no `.qrc` or code change**; `trace.ico`/`trace.icns` compiled from
the delivered PNG sets mirroring the existing container structure. `volume`/`loop` stayed in
`source/` only — those features do not exist and `--strict` fails on them on purpose.

Verified: asset check `--app-icon --strict` exit 0 · build green, new payloads confirmed inside
the built exe by byte search · cross-backend `overlay.ps1` `08-mid-drag` **0 px, max delta 1**
(no pixel-snapping needed) · `banddiff.ps1` bar mode 0.12%/max 29, the video band's own class.

**One harness trap found and it generalises: `powershell -File script.ps1 -Env "A=1","B=2"`
flattens the array into one garbage token.** `TRACE_RENDERER` got an unknown value and fell
back to cpu, `TRACE_TRANSPORT_BAR` was swallowed, and the first "cross-backend" diff compared
cpu with cpu and read a perfect 0 — an instrument *exonerating* wrongly, which is the harder
direction to notice. Caught by reading `renderer cpu +overlay` off both HUDs. Invoke
`restart.ps1` with `&` from inside PowerShell so the array survives, and **read the HUD's
`renderer` field off both captures before believing any cross-backend diff.**

---

# Previous state (2026-08-15): v0.2.0-beta.1 shipped, no open owner decision at that time.

Trace is **beta** as of 2026-08-15. The named gate — mixed-monitor DPI — closed on
hardware at `8945894`, which is what promoted the release.

The 8K ProRes investigation is **closed** (owner, 2026-08-15). **Do not start stage
two, do not start CUDA/NVDEC, and do not start any new playback-architecture work.**

---

## THE RELEASE

**Tag `v0.2.0-beta.1`**, commit `066d7c9`, 36 commits since `v0.2.0-alpha.1`.
Published as a GitHub prerelease with a **40.8 MB** ZIP; the body is
`docs/release-body.md`.

**The tag build was green and all verification steps were read individually
rather than taken off the summary** (run `31919458108`, both caches hit):

| step | output |
|---|---|
| minimal FFmpeg output + dependency gate | `20.8 MB` · `all DLLs import only Windows system libraries` |
| FFmpeg found by CMake | `FFmpeg detected` · `Audio dependencies detected` |
| package launchable | `6 required files present, 95.3 MB total` |
| renderer initializes | `renderer=d3d11 fellback=0 planar=1` |
| window-shape geometry | `OK - 11 shapes x 4 scale factors` |

**`fellback=0` means the runner took the HARDWARE D3D11 path, not WARP.** The check
accepts `d3d11 (warp)` by prefix, so a WARP pass would have looked the same in the
step status and different in this line — which is the reason to read the output.

**Shipped with `TRACE_PLAYBACK_QUEUE` default off.** That is the configuration every
regression figure in the release was measured on.

**Also default off and named in the notes as untested rather than as a fix:**
`TRACE_IO_READAHEAD`. It is correctness-verified and measured only against an
injected synthetic delay; **no live remote mount was available**.

### THE RELEASE STAGE IS NOT IN THE VERSION NUMBER

CMake's `VERSION` field cannot hold a prerelease suffix, so `project(Trace VERSION
0.2.0)` covers **both** the alpha and the beta of this line and the *word* is what
distinguishes them. It is a literal in `src/app/MainWindow.cpp` **three times**:
`buildIdentity()`'s `Trace %1 (beta)`, the About dialog's small print, and the
**Report an Issue mail subject**. **Verify against the built binary, not the source.**

### The package is `trace-windows-x64` and carries no release stage

Renamed 2026-08-15 (`3b732f0`). It is `DIST_NAME` at the top of the workflow and is
spelled **once**. **Two `trace-alpha` strings must NEVER be renamed and a blind
search-and-replace hits both**: the remote `bigsbypuglise/trace-alpha`, which is the
repository, and `docs/release-notes-alpha.md`, whose filename is kept so links do not
break. Published assets up to `v0.2.0-beta.1` were not renamed retroactively.

---

## THE 8K LIMITATION, AS MEASURED — unchanged, and stated in the release notes

**File:** `12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov` — 7680x4320
ProRes 4444 XQ, `yuva444p12le`, 23.976 fps, 5739 Mbps.
**Machine:** AMD Ryzen 9 5900XT, 16 cores / 32 logical.

| | fps | % of 23.976 |
|---|---|---|
| Trace, minimal FFmpeg build, default threads, queue off | **13.64** | **56.9%** |
| Trace, same + stage-one queue at depth 2 | **14.87** | **62.0%** |
| Trace, shipping vcpkg build, queue off | 12.72 | 53.0% |

**Decode is the binding term and is already at its knee.** `dec 39.08ms` against a
`41.71ms` budget — **94% of the whole frame**. The curve is **flat from 32 threads to
64**. **The limit is per-core throughput and memory traffic, not parallelism.** Sweep:
`docs/8k-decode-threads-sweep.md`.

**Stage two is not justified by the margin.** A *perfect* three-stage pipeline gives
**25.6 fps at zero contention** — a 6% margin over 23.976 — against contention stage
one actually measured (`sws` **+24%**, `upload` **+91%**).

**`TRACE_RT_DROP` is an emergency fallback and must never be quoted as the 8K
result.** The owner rejected the frame-drop outcome on 2026-08-13.

---

## PRESERVE THIS DISTINCTION: standalone decode throughput is NOT presentation throughput

| what is measured | figure | what it includes |
|---|---|---|
| **Standalone FFmpeg decode** (`decbench`, minimal build, t=32) | **25.24 fps** | decode only. Demux subtracted. |
| **Trace's own `dec` term** (in process, minimal build) | **39.08 ms = 25.6 fps equivalent** | decode only, inside Trace |
| **Trace presentation throughput** | **13.64 fps** | decode + conversion + upload + paint, strictly serial |

**The first two agreeing within 1.5% is the useful result.** The whole gap to 13.64 is
the rest of the pipeline being serial.

---

## SETTLED — do not change any of these without an owner decision

- **Decode thread policy: `av_cpu_count()` clamped to 64 for intra-only; FFmpeg's
  automatic count for long-GOP.** Derived from the machine, never hard-coded. The HUD
  reads **`thr slice x32`** / **`thr frame x16`**.
- **The async exact landing stays as it is** (`cc8e638`). `TRACE_ASYNC_LANDING=0` is
  the in-binary control. **Rapid steps coalesce** — a stated behaviour change.
- **Stage one stays DEFAULT OFF** (`TRACE_PLAYBACK_QUEUE=0`).
- **`TRACE_IO_READAHEAD` stays DEFAULT OFF** until validated against a real remote mount.
- Nearest magnification above 1:1 and its point-sampled chroma · the pan's behaviour ·
  Fit to Window taking no default shortcut and staying enabled while checked ·
  `kFadeMs`, `kAutoHideMs` and the 460x84 panel with its 44/34 controls ·
  `kMinPlaybackSpeed`, `audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and
  `frameToRgbImage`'s own swscale context · Loop persisting across a file change and a
  restart · the settings home · the §4 opening-size cap · `d3d11` as the default
  renderer · the 384 MB reverse-cache budget · the accessibility proxies staying
  `Qt::NoFocus` and out of the tab chain.
- **The empty state's geometry is the design package's, not a guess** (2026-08-18): a
  104 logical px mark, a 22px gap, a 14px line at `rgba(255,255,255,0.42)`, centred as
  a column, with the mark's canvas centred rather than its ink. Changing one reopens a
  design decision.

## EXPLICITLY DEFERRED — not cancelled, not started

- **The empty state's prism animation.** Optional, and the roadmap does not require it.
  See the 2026-08-18 block above for why it is an owner decision rather than a
  tidy-up: the committed PNG renditions and an in-code gradient animator are
  alternatives, not layers.
- **Checkpoint 2 stage two** (three-stage pipeline). Re-open only with a reason the 6%
  margin survives contention.
- **Turning stage one on by default.** Needs an owner decision, not more work.
- **Hardware/GPU decode (CUDA/NVDEC).** Explicitly excluded by the owner.
- **The 8K plate's acceptance.** Unmet and now understood.
- **Step 10, 10-bit display output** — two external gates, unchanged.
- **LucidLink read-ahead validation against a real mount.**
- **The 720p ComfyUI subjective comparison against QuickTime** — must be **at the
  machine, not over Parsec**.
- ~~Mixed-monitor DPI residue.~~ **WITHDRAWN by owner decision, 2026-08-15.** The
  second display is disconnected and **no multi-display work is to be proposed.**

---

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Fourteen
instances — the fourteenth is the prism animation's "small gradient animator", which is
only true under a different shipping decision than the one taken.

**A recorded figure is a record, not a baseline, unless a control was taken beside it.**
Added 2026-08-18: a 100.0% cadence figure from the previous session read 99.1% today on
a binary built from its own commit. Seen again the same day, the other way round — the §4
default window with the HUD forced on is bistable and moved `win 728x795 → 1278x1083` on
BOTH binaries.

**Standalone throughput is not presentation throughput.**

**A validated PREDICTION is not a validated MECHANISM.**

**Check what a number is measured against before believing it.**

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Twelve times, two of which *exonerated* one. The
eleventh and twelfth are step 7's: `overlay.ps1` reporting a 1253x675 "panel" and
`emptystate.ps1` a 1154x470 "mark", both because a detector met chrome it was not written for.

**Names lie; read the definition.**

**A negative grep is only evidence if the thing would have to be in that file.**

**All N cases failing the same way is a statement about the harness's inputs.** Seen
again on 2026-08-18: `overlay.ps1` reported "panel not found" on both backends because
a single-quoted `-OutDir '$env:TEMP\...'` never expanded and the directory could not be
created.

**Reproduce on the reported case AND on a healthy one before theorising.**

**`git add` succeeding is not evidence a file was added — `git ls-files` is.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD`. **`gh` is NOT installed**; the
  git credential helper holds a usable token.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes
  from **`docs/release-body.md`**.
- **CI has FIVE verification steps and they are separate claims.** Read them
  individually — a green summary is not five green checks.
- Build with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. **Stop a running
  `Trace.exe` first** or the link fails with LNK1104. `build/` is vcpkg (shipping);
  `build-ffci/` is the same tree with the minimal GCC FFmpeg.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()`**,
  so use `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it
  mangles every `§` — use the Write tool. **A `git commit -m` here-string containing
  `>` or `->` fails**; write the message to a file and use `git commit -F`.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML, and so is every SVG
  master — resvg enforces it where some renderers do not.
- **Rasterising an SVG on this box: `pip install resvg-py`.** There is no `magick`,
  `inkscape` or `rsvg-convert`, and `cairosvg` installs but cannot load a cairo DLL
  (pycairo statically links its own). `resvg_py.svg_to_bytes(svg_path=..., width=N,
  height=N)` returns PNG bytes and handles gradients and `mix-blend-mode`.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @
  239.999Hz. **No subjective smoothness, cadence or picture judgement is valid over
  Parsec at all.**
- **PARK THE MOUSE CURSOR ON THE PRIMARY BEFORE ANY MEASUREMENT.** Quote `scr`.
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.**
- The asset set is at **`C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets`**.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`,
  `transitions.ps1` (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`,
  `scrub.ps1` (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run
  both legs**), `previewshot.ps1`, `clickland.ps1`, `stepcost.ps1`. Most take no
  `-Clip`: **`restart.ps1` first**, and **`widen.ps1` after it** on portrait media *and*
  on 16:9 at this display, or the groove scan fails.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`,
  `overlay_press.ps1`, `overlay_ladder.ps1`.

  *Needs NO media at all, and is the only one that does*: **`emptystate.ps1`** — modes
  `launch` / `transport` / `close` / `swap`, `-Bar` for the escape-hatch control, and it
  launches Trace itself rather than through `restart.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs
  `TRACE_NO_AUDIO=1` for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `widen.ps1`, `viewscale.ps1`, `inspector.ps1`, `uiatree.ps1`, `phase14.ps1`,
  `menushot.ps1`, `recentfiles.ps1`, `resizecache.ps1`, `swapexe.ps1`, `banddiff.ps1`,
  `abfilter.ps1`/`croprect.ps1`, `decthreads.ps1`, `strip.ps1`.

  *Needs two displays at different scale factors*: **`dpimove.ps1`** — **the hardware
  is disconnected; do not propose work that needs it.**
- **Do not run `transitions.ps1` on a 9:16 clip.** Its own header records that
  pillarboxing produces PASSes that mean nothing.
- **Prefer an in-binary control to a control build.** Where a control build is
  unavoidable, check out the parent commit and rebuild in the same build directory —
  a copy of `Trace.exe` elsewhere has no Qt DLLs beside it and will not start — and
  **verify every swap by hash**.
- Update `CLAUDE.md` and the plans at the end of the session.
