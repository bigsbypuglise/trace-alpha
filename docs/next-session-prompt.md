# PHASE 15 IS SIGNED OFF. PHASE 16 — the full regression — CLOSES THE PASS.

## PHASE 14'S TWO REMAINING OWNER ITEMS PASSED (owner, 2026-08-11). RECORD THEM FIRST.

**The menus and Help wording are ACCEPTED**, including phase 15's scaling entries. That was
the last wording question in the pass.

**THE NARRATOR LISTEN IS DONE AND PLAN §31.5 ITEM 4 IS CLOSED.** Narrator discovers the
floating transport; **Play/Pause, Rewind, Fast-forward, Timeline and Share are announced
clearly and activate with Narrator+Enter**; and ordinary Space play/pause is unaffected. So the
composited transport is genuinely usable by a screen reader, not merely present in the UIA
tree — which is the distinction every prior record was careful to keep open, and it is now
answered by listening rather than by walking.

**Document HOW it is reached, because it is not the obvious way and a future session will
otherwise read it as a defect.** The renderer-drawn transport is navigated with the **Narrator
cursor**, not the ordinary Tab chain. That is deliberate: phase 14 made the proxies
`Qt::NoFocus` after the first cut gave them `Qt::TabFocus` and the Rewind proxy took initial
focus, so **the first Space press on a fresh launch read `speed -2.00x`**. Screen readers
navigate the accessibility tree rather than the tab chain, and every command already has a
shortcut that works from anywhere — so keeping the transport out of the tab chain protects
playback keyboard focus and costs a screen-reader user nothing. **Do not "fix" this by putting
the proxies back in the tab chain**; that is the regression `eeea986` folded in.

With these, **every owner item in the interface pass is answered** and phase 16's part (b) is
already satisfied. Record the sign-offs in `CLAUDE.md`, `docs/interface-pass-1-progress.md` and
plan §31.5 before starting the regression, each at its stated width.

## AFTER THE REGRESSION PASSES — cut a release, and it STAYS ALPHA

**Owner decision, 2026-08-11: this is not the first beta.** The interface pass is
feature-complete against the approved spec and every owner item is answered, but **real
mixed-monitor DPI has never executed** — no `WM_DPICHANGED`, no monitor-to-monitor move, no
fullscreen on a secondary display; §4's whole window-shaping system takes `dpr` as an argument
and has only ever been driven synthetically (§20.4). Trace's audience is VFX and motion
artists, where multi-monitor at mixed scaling is close to universal, so that gap is exactly
what a wider audience would hit first. **A second display is being set up later in the week**;
beta follows that, not this release.

**Tag `v0.2.0-alpha.1`.** The newest existing tag is `v0.1.0-alpha.23`, and continuing that
sequence would understate what shipped — phases 6–15 are an entire new interface surface. The
minor bump marks the milestone while staying honest about maturity, and it leaves the obvious
next step legible: **`v0.2.0-beta.1` once mixed-monitor DPI is validated.**

**Write the release notes to name the known gaps plainly**, because an alpha with honest limits
gets useful bug reports and one without gets reports about things already known. At minimum:
EXR does not open (`TRACE_WITH_OIIO` is undefined in vcpkg and CI); HDR / BT.2020 has no
tonemap, so that content looks wrong on both backends; 10-bit **output** is not supported
(§9 — not to be confused with the high-bit-depth *processing* that shipped at GATE C);
mixed-monitor DPI is unvalidated; and cold LucidLink delivery is ~600–800 Mbps, so multi-Gbps
plates will not stream. Note also that Windows ships as a **portable ZIP with no installer** by
deliberate choice, and that `TRACE_RENDERER=cpu` is the escape hatch if anything about the
picture looks wrong.

A `v*` tag publishes a real ZIP asset and marks the GitHub release prerelease — that is
correct here and should stay. Verify the tag build is green before announcing it, the way the
last few sessions have read the three verification steps individually rather than trusting the
overall conclusion.

**PHASE 15 SHIPPED AND IS ACCEPTED (2026-08-11): Actual Size, Fit to Window, Zoom In, Zoom Out
and the pan they imply.** `aae42bf` render · `55fd965` app · `163c439` harness · `9315af0`
the Fit-to-Window correction and the record. Full record in
`docs/interface-pass-1-progress.md` under "Phase 15".

**What was accepted, at its stated width:** the **scaling behaviour, the magnification filter,
the pan, and the zoomed-scrub trade**. Four things are now settled rather than default —
**nearest above 1:1 stays** (so the point-sampled chroma is accepted behaviour, not a carried
defect, and `TRACE_MAG_FILTER=linear` is a comparison knob rather than a pending revert path);
**the pan's behaviour is correct**; **Fit to Window takes no default shortcut**; and **the
under-resolved preview during zoomed scrubbing is accepted** on the stated condition that the
exact full-resolution image returns immediately on release **with no position jump**. Changing
any of them re-opens an owner decision.

**It is NOT a sign-off on the menus and Help wording** — phase 14 left those open and phase 15
added four rows to them — and **the Narrator listen is still owed** (plan §31.5 item 4). Those
two are the only owner items left in the whole pass, and phase 16 cannot honestly close it over
them.

**One correction was required and applied before sign-off: Fit to Window stays ENABLED while
active and shows its checked state.** The first cut greyed it while already fitting, reasoning
that a command which visibly does nothing is the `showInfo` failure phase 2 deleted. **That
reasoning does not transfer to a CHECKABLE item** — the tick is what states the current state,
and greying the row makes "this is what the picture is doing" read as "this is unavailable".

**One item leaves phase 15 as POLISH with its condition stated:** a grab / closed-hand cursor
while a pan is possible. It would improve discoverability, is **not required**, and is wanted
only if it can be made trivial **and identical on both backends**. It cannot today — the D3D11
surface owns its own window-class cursor and answers `WM_SETCURSOR` itself while the CPU path
inherits the widget's, the same two-mechanism split `setCursorHidden()` already carries. **Do
not describe it as a one-liner.**

---

## What is accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10)** — the engine, at 2×/5×/10×/30× in both directions.

**Phases 6, 12, 13 and 15 are signed off** — the floating transport's feel and identity; the
media-shaped window (stills and image sequences re-signed-off on the corrected build at
`3a38516`); the Movie Inspector's contents, wording and origin labels; and view scaling's
behaviour, magnification filter, pan and zoomed-scrub trade.

**Phase 14 is PARTLY signed off (2026-08-11): Loop, 0.5× and Copy Current Frame are ACCEPTED.**
They are shipped features now, not extractable candidates, so `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context are **settled behaviour** — changing one re-opens an owner decision rather than being a
tidy-up. **The menus and Help wording, and the Narrator listen, are NOT covered by that.**

**Read each at its stated width.** A later summary that says "the transport is done" has widened
it.

---

## Owner items outstanding — TWO, both from phase 14

Everything else in the pass is accepted. These are what phase 16 has to close over.

1. **Visual review of the menus and Help.** Trace Help has **real content written for phase 14**
   — four paragraphs, and the only prose in the product. It is worth reading. Note phase 15 added
   four rows to the View menu and two to the Window menu since that review was first asked for,
   so this is now a review of the menus **as they now stand**.
2. **THE SCREEN READER HAS NOT BEEN LISTENED TO.** `uiatree.ps1` proves the transport is
   *exposed*: five controls, correctly named, correctly typed, on the drawn rects, and
   **invokable** through UI Automation with no focus and no click. **UIA is the interface
   Narrator consumes, so an element absent from that tree is certainly not announced — but an
   element present in it can still read badly, in the wrong order, or with a name that is
   nonsense aloud.** Plan §31.5 item 4 stands. Narrator ships with Windows and is on this box;
   this needs ten minutes and a person.

**And one question rather than an item: Loop's persistence.** It survives a file change and a
restart, deliberately — a review preference, not a property of the media. It was accepted with
the feature, so this is only worth raising if it turns out to surprise anyone.

**Carried as polish, not as work:** the pan cursor, with the condition in the header above.

---

## PHASE 16 IS THE OPEN PHASE: the full regression that closes the interface pass

It is not a formality and it is not just re-running the harness. Three things belong in it.

**(a) The standing regression on the whole asset set, not the two files each phase used.**
Every phase since 6 has run 4K H.264 cadence, 4444 cadence, `-SnapRelease`, both lifecycle legs
and the 25 transitions. The pass has never been re-measured across 1080p, 4K 60fps, 422 HQ, the
1×1, the 4×5 and the image sequence *since the interface work began*. Phase 15 moved the fit,
which drives the preview size and therefore cache depth (§22.8) — quote `display` **and**
`win WxH` on everything.

**(b) The two owner items above.** Phase 16 cannot close the pass over unread menus and an
unheard screen reader.

**(c) A read of what the pass actually shipped against the spec.** Fifteen phases have each
recorded their own scope; nobody has checked the union against `docs/interface-pass-1-spec.md`
end to end. Expect at least one item that was deferred by a phase and never picked up —
`Ctrl+0` was carried for five phases before phase 15 took it.

**AND THERE IS ALREADY ONE CONFIRMED, WHICH IS WHAT MAKES (c) THE HIGHEST-VALUE PART OF THIS
PHASE RATHER THAN A FORMALITY. THE MOVIE INSPECTOR STILL HAS NO DURATION ROW.** `CLAUDE.md`
records the phase 14 owner ruling verbatim — *"the inspector's carried Duration row is ADDED,
origin `encoded` because it is the container's claim"* — and it was never implemented. Verified
at HEAD: `grep -rni "duration" src/app/MovieInspector.cpp` returns nothing, and the General
section's labels in `MainWindow.cpp` (~line 2215) run File name, Source path, Resolution, File
size, Overall data rate with no Duration among them. **`VideoDecoderFFmpeg.h:29` already carries
`durationSeconds`, read at open**, so it is the one-row addition the ruling predicted.

Note what kind of miss this is. It is not a spec item that was skipped — the spec's own field
list for the inspector never asked for Duration. It is an **owner ruling made during a phase and
recorded in three documents without being built**, which no phase record would surface, because
each record describes what its phase did. So run (c) against **the spec AND the owner rulings**,
not against the phase records: read `CLAUDE.md`'s decision entries and each progress section's
sign-off text looking for verbs like *added*, *accepted*, *is ADDED* that no commit implements.

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
15. **View scaling** — Actual Size, Fit to Window, Zoom, pan. **DONE and SIGNED OFF**, after
    one correction (Fit to Window stays enabled while checked).
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
picture-quality judgement is valid over Parsec at all** — which included every phase 15 owner
item, all of which were therefore taken at the machine.

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
