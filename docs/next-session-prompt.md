# PHASE 15 IS BUILT AND MEASURED. PHASE 16 — the full regression — CLOSES THE PASS.

**PHASE 15 SHIPPED (2026-08-11): Actual Size, Fit to Window, Zoom In, Zoom Out and the pan they
imply.** `aae42bf` render · `55fd965` app · `163c439` harness. Full record in
`docs/interface-pass-1-progress.md` under "Phase 15".

**NOTHING IN PHASE 15 IS SIGNED OFF.** Everything is measured; the magnified picture, the pan's
feel and the menu wording are owner items and nobody has looked at them. See "Owner items
outstanding" below — it is the *first* thing to get answered, because phase 16 cannot honestly
close the interface pass over two phases of unaccepted work.

---

## What is accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10)** — the engine, at 2×/5×/10×/30× in both directions.

**Phases 6, 12 and 13 are signed off** — the floating transport's feel and identity; the
media-shaped window (stills and image sequences re-signed-off on the corrected build at
`3a38516`); and the Movie Inspector's contents, wording and origin labels.

**Phase 14 is PARTLY signed off (2026-08-11): Loop, 0.5× and Copy Current Frame are ACCEPTED.**
They are shipped features now, not extractable candidates, so `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context are **settled behaviour** — changing one re-opens an owner decision rather than being a
tidy-up. **The menus and Help wording, and the Narrator listen, are NOT covered by that.**

**Read each at its stated width.** A later summary that says "the transport is done" has widened
it.

---

## Owner items outstanding — phases 14 and 15 together

**Phase 14, still open:**

1. **Visual review of the menus and Help.** Trace Help has **real content written for that
   phase** — four paragraphs, and the only prose in the product. It is worth reading.
2. **THE SCREEN READER HAS NOT BEEN LISTENED TO.** `uiatree.ps1` proves the transport is
   *exposed*: five controls, correctly named, correctly typed, on the drawn rects, and
   **invokable** through UI Automation with no focus and no click. **UIA is the interface
   Narrator consumes, so an element absent from that tree is certainly not announced — but an
   element present in it can still read badly, in the wrong order, or with a name that is
   nonsense aloud.** Plan §31.5 item 4 stands. Narrator ships with Windows and is on this box;
   this needs ten minutes and a person.
3. **Loop's persistence.** It survives a file change and a restart, deliberately — a review
   preference, not a property of the media. Say if that is wrong.

**Phase 15, all of it:**

4. **The magnified picture.** Nearest at 2:1, 4:1 and 8:1. **Look specifically at the chroma**:
   the magnification sampler point-samples the chroma planes too, so on 4:2:0 media a colour
   edge steps in 8×8 blocks at 4:1. That is honest — a chroma sample really does cover four luma
   samples — and it is **the most likely thing in this phase to read as a defect while being
   correct.** `TRACE_MAG_FILTER=linear` is the side-by-side, on both backends, and it is also
   how the whole nearest decision gets reverted without touching anything else.
5. **The pan's feel.** Direction, rate, and the clamp at the edges. **There is no cursor change**
   while a pan is possible: the D3D11 surface owns its own class cursor, so a grab cursor is two
   mechanisms rather than one, and it was left out rather than half-done. Say if it is wanted.
6. **Fit to Window is disabled while already fitting**, showing as ticked-and-greyed. That is the
   design package's Disabled state, but it is an affordance choice nobody has read.
7. **Fit to Window has no shortcut**, by the phase 10 rule — the spec's Keyboard section names
   `Ctrl+0` and `Ctrl+plus`/`minus` and nothing else, and a key claimed here would have to be
   given up later. Say if it should have one.
8. **The preview is under-resolved at a zoom.** The preview is sized to the *viewport*, but a
   zoomed viewport shows a *crop*, so at Actual Size on 4K the mid-drag picture carries 1066x600
   for the whole frame while the visible region is ~296x166 of real source. Soft during motion,
   exact on release — the standing rule rather than a new exception, but nobody has watched it.

---

## PHASE 16 IS THE OPEN PHASE: the full regression that closes the interface pass

It is not a formality and it is not just re-running the harness. Three things belong in it.

**(a) The standing regression on the whole asset set, not the two files each phase used.**
Every phase since 6 has run 4K H.264 cadence, 4444 cadence, `-SnapRelease`, both lifecycle legs
and the 25 transitions. The pass has never been re-measured across 1080p, 4K 60fps, 422 HQ, the
1×1, the 4×5 and the image sequence *since the interface work began*. Phase 15 moved the fit,
which drives the preview size and therefore cache depth (§22.8) — quote `display` **and**
`win WxH` on everything.

**(b) The owner items above.** Phase 16 cannot close the pass over unaccepted work in 14 and 15.

**(c) A read of what the pass actually shipped against the spec.** Fifteen phases have each
recorded their own scope; nobody has checked the union against `docs/interface-pass-1-spec.md`
end to end. Expect at least one item that was deferred by a phase and never picked up —
`Ctrl+0` was carried for five phases before phase 15 took it.

**Known to be OUT of the pass and staying out**: Check for Updates (no updater exists),
10-bit output (§9, two external gates), mixed-monitor DPI (§20.4, no hardware), BT.2020 tonemap,
LucidLink read-ahead, EXR/OCIO.

---

## What phase 15 leaves for anything that touches the picture

- **`viewDeviceRect()` is the one destination-rect expression and the FIT BRANCH IS THE OLD
  ARITHMETIC, reached by one test.** Do not "unify" it. A version that fitted by computing a fit
  scale and running it through the scale path would be the same answer by a different route, and
  every recorded fit figure in this project would then be comparing against arithmetic it was
  not taken on.
- **The scale's reference is the FULL-RESOLUTION SOURCE, never the delivered frame.** A backend
  holds a *preview* during a drag — 1066x600 standing in for 3840x2160 — so scaling what it
  holds would make the picture jump 3.6× on release. `ViewScale::referenceDisplayed` is computed
  once, in `ViewerWidget::applySourceShape()`, beside the transform composition.
- **`maxViewScale()` is a hard backend limit, not a taste judgement.** D3D11 letterboxes by
  **viewport**, and a viewport past `D3D11_VIEWPORT_BOUNDS_MAX` is **rejected and draws nothing
  at all, silently.** The CPU backend honours the same cap deliberately: an escape hatch that
  zooms further than the shipping renderer is what nobody would think to check.
- **A view gesture on the picture goes through `OverlayHooks`, not through `ViewerWidget`'s
  mouse handlers.** Under the D3D11 default the video is a child HWND that takes every mouse
  message and Qt's widget never sees one, so anything written there works on
  `TRACE_RENDERER=cpu` and does nothing on the shipping build. Two gestures already live there
  for this reason — double-click-to-fullscreen and now the pan — and both are **ungated on the
  overlay's `enabled_`** so they survive `TRACE_TRANSPORT_BAR=1`.
- **`repaint()`, not `update()`, after anything the paint MEASURES.** `lastDrawSize`, the fit and
  the reduction taps are all measured *by* the paint and reported after it, so a HUD refresh
  following a merely-scheduled repaint prints the previous state and a paused file never
  corrects it. Phase 10 found it, phase 15 found it again in the same shape.

---

## Two harness findings from phase 15 that apply to everything

- **A WINDOW CAPTURE CONTAINS PIXELS FROM OUTSIDE THE WINDOW.** `GetWindowRect` includes Windows
  11's invisible resize border, so the first captured columns are **whatever is behind the app**.
  `transitions.ps1`'s control scan found a fourth cluster at x=3 and reported "groove or controls
  not located" **for all 25 cases**, against a build whose controls were exactly where they
  always are. It is neither clip- nor build-dependent — **it depends on what is on the desktop**,
  which is why the same matrix passed minutes earlier on another file. Any scan that starts at
  x=0 or y=0 of a window capture has this.
- **THE CLIP IS PART OF THE MEASUREMENT — and this time the harness's own header said so.**
  `transitions.ps1` on the 121-frame 422 HQ clip FAILs `F -> ffBtn` with `moved 0%`, exactly as
  its header warns ("121 frames is not [long enough]"). **What told that apart from a phase 15
  regression was a control binary built from `9db4780` in a worktree, which failed the same case
  with the same `0%` and `5.6%` to the digit.** Build the control; do not reason about it.
  `M&M_TopGun_1080.mp4` is the clip that harness needs.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight, fast,
   smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. ~~**No interface work.**~~ **LIFTED by the owner, 2026-08-10.** Priority 1 is the binding
   constraint *on this work*: every phase runs the playback and scrub regression.
3. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. Five instances: the drag preview, §15's scrub sampling,
   accelerated reverse, accelerated forward, and phase 4's shuttle-press decision. **Do not
   re-open any of them on picture-quality grounds alone.** Phase 15's under-resolved preview at a
   zoom is the sixth, and it is covered by this rather than being a new exception.

## Deferred, with the conditions attached

- **Step 10, 10-bit display output** — two external gates, both outside the code.
- **Mixed-monitor DPI (§20.4)** — tabled for hardware; `AllScreens` returns one display and
  Parsec replaces it rather than adding one. **Not executable on this box at all.**
- **BT.2020 has no tonemap** (§22.7 item 5). Known gap, never a complaint.
- **GATE E step 2 — vsync snapping** — stopped by owner decision. **Do not start it without a
  specific new cadence complaint.**
- **LucidLink read-ahead.** Two designs measured worse. Not in progress.
- **EXR / image sequences and OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI.

## The phase list

12. **Spec §4, media-driven window size** — DONE, signed off.
13. **Movie Inspector** — DONE, signed off.
14. **Menus, help, accessibility, Loop, 0.5×, Copy Current Frame** — DONE. **Loop, 0.5× and Copy
    Current Frame ACCEPTED; menus, Help and the Narrator listen still pending.**
15. **View scaling** — Actual Size, Fit to Window, Zoom, pan. **DONE and measured; NOT signed
    off, every owner item open.**
16. **Full regression.** **The open phase.** It can then honestly close the interface pass.

## Priority 1 is the constraint on all of it

Phase 15's regression, on the **physical panel, 5120x1440 @ 239.999Hz**: 4K H.264 cadence ×3
**100.0%** with 120 frames, `handler>budget 0 of 119` and all 119 gaps in the ~1× bucket; 4444
×2 **99.8%** at 0 of 260; `-SnapRelease` `target 120 shown 120 delta 0` full-res planar with
`hitch 0` and `land 0`; both lifecycle legs; **25 of 25 transitions**. Case for case with phases
12, 13 and 14.

**Build the control binary in a `git worktree`, not by stashing**, and **verify every swap by
hash** (`swapexe.ps1`). `windeployqt` the control, and copy the `av*`/`sw*` DLLs across.

## The rules this project keeps re-learning

**A validated PREDICTION is not a validated MECHANISM.** Phase 14's proxy tree had been written
down and cited for eight phases, and building it exactly as described **broke the Space bar**.

**A deferred item's premise expires. Re-derive it before building it.** Nine instances.

**Check what a number is measured against before believing it.** `frames 30 | elapsed 1.25s`
read `100.0% of real time` on a looping file.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Six times now: menu-icon luminance, the
un-refreshed HUD after the LucidLink probe, the HUD after a view transform, the HUD on resize,
P/Invoke's ANSI marshalling, and the HUD after a view scale. **Two of those six are the same
`update()`-instead-of-`repaint()` mechanism.**

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**Reproduce on the reported case AND on a healthy one before theorising.**

## `d3d11` is the default renderer, and two obligations follow

`TRACE_RENDERER=cpu` is the control and the escape hatch — **the first thing to try if anything
about the picture looks wrong** — but it is now the *softer* picture as well as the slower one,
since step 9 only fixed the GPU path.

**Every scrub and playback baseline taken before 2026-08-10 was on `cpu`** and most are not
tagged with a renderer. Valid as records; **not** valid as comparisons against a run taken
today.

## Parsec — ask which display a session is on before comparing any number

Remote sessions present **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @ 239.999Hz**.
`scripts/measure/refresh.ps1` reports the current one — **run it at the START of a session and
again before quoting anything.** `stalls` is `2 × refresh`, so its bar moves 33.3ms → 8.3ms and
**no stall figure crosses that boundary.** `hitch` does. **No subjective smoothness, cadence or
picture-quality judgement is valid over Parsec at all** — which now includes every phase 15
owner item above.

## Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either

`stalls` counts paint gaps over `2 × refresh`, so the same run reads `stalls 51 … | hitch 3`.
**`hitch` is a fixed 33ms bar and is the only stall figure comparable across sessions.** Cache
depth follows the *video rect*, not the window, and both `H` and now a **zoom** change one
without the other.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token.
- **CI asserts the renderer initializes** and **the window-shape geometry across DPI**.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop a
  running `Trace.exe` first** or the link fails with LNK1104.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()` macros**,
  so `std::max`/`std::min` do not compile in `src/render/VideoRenderer.cpp`. Use `qMax`/`qMin`
  there — the file's own comment says so and phase 15 hit it anyway.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§`. Use `cat` from the Bash tool. **A `git commit -m` here-string containing `>` or `->`
  fails** — write the message to a file and use `git commit -F`. **Do not pipe a measurement
  script through `Select-Object -First N`**: that raises `StopUpstreamCommandsException` and
  terminates the script mid-run, which looks exactly like the script crashing. **And
  `Write-Output` from inside a harness function goes down the caller's pipeline** — a `| Out-Null`
  on the return value swallows the report too, so use `Write-Host` for anything the run prints.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- Harness, **and note which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**; the **412-frame** clip for `-LadderOut`),
  `shuttleland.ps1`, `scrub.ps1` (`-SnapRelease` for anything about the landing),
  `lifecycle.ps1` (**run both legs**), `previewshot.ps1`. Note these take no `-Clip`:
  **`restart.ps1` first**.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**passes a scratch `TRACE_SETTINGS_FILE` since phase 14**),
  `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `sidebyside.ps1`, `stalls_vs_window.ps1`,
  `abfilter.ps1`/`croprect.ps1`, `recentfiles.ps1`, `inspector.ps1`, `swapexe.ps1`,
  `make_timecode_fixtures.ps1`, `make_shape_fixtures.ps1`, `resizecache.ps1`, `uiatree.ps1`
  (**run it under `TRACE_TRANSPORT_BAR=1` as the negative control**), `phase14.ps1`,
  `menushot.ps1`, and **new at phase 15: `viewscale.ps1`** (`ladder`/`actual`/`pan`/`filter` —
  **`-Mode pan` is the only leg with a negative control**).

  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the window
  at native resolution (`capture.ps1`), and **check nothing overlapped it**. A diagnostic
  screen-grab must cover the **whole virtual desktop** — a 1920x1200 grab of the top-left
  corner of this panel contains desktop icons and no application.
- Update `CLAUDE.md` and the plans at the end of the session.
