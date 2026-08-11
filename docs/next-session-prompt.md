# The interface pass is open, phase 9 shipped, and phase 10 is next.

Supersedes the previous version. **Spec phase 6 shipped and is signed off by the owner** — the
floating overlay is the only transport, the docked bar is out of the layout behind
`TRACE_TRANSPORT_BAR=1`, and fullscreen is consolidated. **Spec phase 7 shipped** — the time
readout is honest (source SMPTE, or elapsed, never one labelled as the other), Trace has its
first text-entry controls, and zero-based numbering is finished. **Spec phase 8 shipped** —
the Share menu, Copy File Path, Show in File Explorer, and the *gate* on Copy LucidLink Link.
**Spec phase 9 shipped** — Copy LucidLink Link works, driven through the installed
integration's own shell command, and Trace never composes a link. **The next thing to do is
spec phase 10, the temporary view transforms, which is wiring only.** Paste everything below
the line into a fresh session in the repo root.

**Two owner decisions were taken on 2026-08-11 and are recorded rather than open.**
Accessibility: **the alpha ships with the overlay invisible to a screen reader, and phase 13
builds an accessibility proxy tree rather than polishing one** — see the loose-ends section,
and estimate phase 13 as construction. And the `SNAP gop 2` keyframe-grid loose end is **no
longer tracked**; it is not to be carried into another session's notes.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. ~~**No interface work.**~~ **LIFTED by the owner, 2026-08-10.** The interface pass is
   now the open phase. Priority 1 is unchanged and is now the binding constraint *on this
   work*: every phase of it must be measured against the playback and scrub baselines, and
   a feature that costs smoothness loses.
3. **Smooth, responsive motion beats matching final-frame fidelity during motion.**
   Fidelity is owed to the frame the user stops on, not to the frames flying past on the way
   there. This settles a whole class of trades in advance — preview resolution, preview
   filtering, sampling stride, paint pacing, shuttle sampling — so **do not re-open any of
   them on picture-quality grounds alone.** Five instances now: the drag preview, §15's scrub
   sampling, accelerated reverse, accelerated forward, and phase 4's decision that a shuttle
   press does not re-decode the frame it is about to replace.

## What has been accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10, owner retest)** — fast-forward advancing clearly through
the complete 2×/5×/10×/30× ladder on every format; reverse 30× reading as intentional at the
approved ~15fps presentation cadence; direction changes responding correctly; stopping
landing on the last visibly displayed frame; and no regression to normal playback, audio
return, scrubbing, exact release or stepping.

**Read each at its stated width.** What was accepted in the shuttle phase is the **engine**.
A later summary that says "the transport is done" has widened it.

## Deferred, with the conditions attached

- **Step 10, 10-bit display output** — two external gates, both outside the code: a
  10-bit-capable display confirmed, and the intended Windows Advanced Color / HDR /
  colour-management workflow defined. §9's warning still holds — this is 10-bit **output**,
  not the high-bit-depth **processing** that shipped at GATE C.
- **Mixed-monitor DPI (§20.4)** — tabled for hardware. `AllScreens` returns one display and
  Parsec replaces it rather than adding one, so this is **not executable on this box at
  all**. Do not re-propose it until a second monitor exists.
- **BT.2020 has no tonemap** (§22.7 item 5). Known gap, never a complaint. §28.4 applies if
  picked up: the shader averages before the matrix because both remaining steps are affine.
- **GATE E step 2 — vsync snapping** — stopped by owner decision, design retained unbuilt at
  §24.4–24.6. `DwmGetCompositionTimingInfo` fails on this machine, so any future E2 is
  d3d11-only via `IDXGISwapChain::GetFrameStatistics`. **Do not start it without a specific
  new cadence complaint.**
- **LucidLink read-ahead.** Two designs measured worse; try full-request buffered serving
  before partial reads, then benchmark. Not in progress.
- **EXR / image sequences and OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI, so EXR
  does not open today. Largest untouched area, and a feature rather than a fix.

## PHASE 6 IS SIGNED OFF — owner, 2026-08-11. Do not re-open it.

The floating transport **passed its visual sign-off**: it clearly reads as the transport, the
**2s inactivity delay feels right**, the **165ms fade feels natural**, and **no tuning is
wanted**. So `kFadeMs`, `kAutoHideMs` and the 460×84 panel with its 44×34 controls at the top
of `src/render/OverlayModel.cpp` are **settled numbers, not defaults** — changing one is
reopening an owner decision.

Read it at its stated width. What was accepted is **the feel of the auto-hide and the panel's
identity as a transport**. It is not a sign-off on the Time Display readouts (phase 7 changes
them), on the menus (phase 13), or on the overlay being finished: **plan §31.5 item 4 stands
— the overlay is not final until a screen reader has driven one.**

## THE OPEN PHASE — interface pass 1, now at phase 10

The owner chose it on 2026-08-10 and lifted priority 2 to allow it. The spec is
`docs/interface-pass-1-spec.md`; the phase record is `docs/interface-pass-1-progress.md`.

### Done on 2026-08-10 — do not redo any of it

- **The spec is renamed and open, and its §2 was RE-DERIVED** (`994dd7b`). Read §2 as it now
  stands, not as the 2026-08-09 text.
- **Both GPU prerequisites are BUILT AND MEASURED** — the renderer-neutral overlay
  (`5e1f834`) and the `VideoRenderer` view-transform contract (`4b7174f`). Plan §31.
- **The spec's phase 1 audit is `docs/interface-pass-1-audit.md`** (`7abb6a5`).
- **Phase 2 shipped at `58bfca6`** — fullscreen as a shared QAction, the dev HUD toggle on
  **`H`**, the dead `showInfo`/`showTimecode`/`showSeconds` flags deleted, and the icon tree
  down to the approved `260807` package.
- **The asset tree is reorganised and every reference re-pointed** (`cbf6d98`).
- **Phase 3 shipped at `4de678e`** — the shortcut table, the extracted `startShuttle()`
  sequence, `ShuttleEntry::AtOneX`/`AtTwoX`, and a real bug in the frame-step button.
- **Phase 4 shipped at `e559d07`** — the forward shuttle interface, the settled
  `landPreviousExactly` decision, and `transitions.ps1`. Full record in the progress doc.
- **Phase 5 shipped** — the reverse shuttle interface. The transport redesign is now
  COMPLETE and the spec's validation list for it reads straight down. Full record in the
  progress doc.
- **Phase 6 shipped on 2026-08-11** — the floating transport replaces the docked bar,
  auto-hide is finished to the spec's list, and fullscreen is consolidated. **Owner sign-off
  passed.** Full record in the progress doc.
- **Phase 7 shipped on 2026-08-11** — real source SMPTE timecode with drop-frame, the readout
  relabelled honestly, Go to Frame / Go to Timecode, and zero-based image-kind HUD lines.
  Full record in the progress doc.
- **Phase 8 shipped on 2026-08-11** — the Share menu on three surfaces from one `QAction` set
  and one `QMenu`, Copy File Path, Show in File Explorer, and the LucidLink gate. Full record
  in the progress doc.
- **Phase 9 shipped on 2026-08-11** — the LucidLink integration is real. The link is produced
  by the installed shell extension, invoked through `IShellExtInit`/`IContextMenu` on the one
  discovered CLSID, and validated before it is accepted. Full record in the progress doc.
- CI run 79 green on `2bb1901`, run 81 on `58bfca6`, run 84 on `cbf6d98`, run 86 on `e559d07`,
  run 87 on `90140f9`, run 88 on `883d216`, run 89 on `fec93f0`, **run 90 on `bc84431`
  (phase 6)**, **run 92 on `f15e368` (phase 7)**, **run 94 on `1bec8c5` (phase 8)** and
  **run 96 on `69a45c1` (phase 9)**, all including the renderer selftest.

### Start at spec phase 10, and read `docs/interface-pass-1-progress.md` first

**Phase 10 is the temporary view transforms, and it is wiring only.** Rotate Left, Rotate
Right, Flip Horizontal, Flip Vertical, Reset View Transform — five `QAction`s onto
`viewer_->setViewTransform()`, plus a reset when new media opens. The renderer-neutral
contract was built and measured back at plan §31 (`4b7174f`); `TRACE_VIEW_TRANSFORM=90|180h|v`
is the interim knob and **it leaves with this phase**, the way `TRACE_SHUTTLE_ENTRY` left with
phase 5.

**Two traps are already recorded and both fail silently.** A quarter turn **re-letterboxes** —
`display 640x360` becomes `202x360`, identical on both backends — and the **reduction taps must
be recomputed from the post-transform fit** with the footprint axes exchanged, or the box
average filters the wrong axis (`filtered x3` → `x4` at rot90). The CPU path names `scale`
before `rotate` deliberately, because QPainter post-multiplies and the other order turns
`rot90 + flipH` into `rot90 + flipV` — the two backends would then differ by a mirror while
every number agreed.

**The spec's rules for it**: viewing transforms only, never modify source media, never write
metadata, never remux; reset on new media; keep the transform through seek, scrub and play; do
not change authoritative frame identity. Menu home is **Edit** per the spec, and phase 7's
precedent applies — put the group where it belongs now and let phase 13 restructure.

**The 1×1 and 4×5 ProRes assets are the right material for it** (`9_1x1_ProRes`,
`10_4x5_ProRes`): a square clip makes a 90° rotation's letterboxing arithmetic visible in a way
16:9 does not, and the 4×5 is the case where a quarter turn changes which axis binds.

### What phase 9 leaves behind

- **`src/app/LucidLinkIntegration.*` is the whole vendor surface.** If LucidLink ever needs
  re-visiting, the two entry points are `probeLucidSupport()` (does the integration offer a
  link for this file) and `copyLucidLinkViaShell()` (run it and validate what comes back).
- **`TRACE_LUCID_LOG=1`** prints one stderr line per probe and copy. Reach for it first: the
  gate is three refusals deep and `lucid disabled` looks identical whether the handler was not
  found, the extension declined the file, or the menu had no matching item.
- **`TRACE_LUCID_COINIT=1`** is the retained control for a closed question — `CoInitializeEx`
  instead of `OleInitialize`, measured identical.
- **The display-string dependency is the one real fragility.** The extension exposes no
  canonical verb, so the item is matched on the text `Copy link` as rendered by
  **LucidShellExt 1.0.15**. A LucidLink update that renames it, or a localized Windows, turns
  the command unavailable — which is the safe direction, because the neighbouring item is
  `Pin` and pinning writes to the mount. **If a user ever reports Copy LucidLink Link greyed
  out on a real LucidLink file, check that string first.**
- **A `V:\` file must be nominated for any future LucidLink testing.** The mount is live client
  production storage and is strictly read-only; phase 9 used only the nominated file and
  invoked only the copy-link command.

### Carried owner visual-review items

- **The Share glyph is a `»` double-chevron** (the approved package's `share_menu`) sitting
  beside Fast-forward's `▶▶`. Shipped as delivered — artwork follows behaviour and the package
  is the approved source — but the two are similar in silhouette. Unchanged by instruction
  during phase 9.
- **The floating transport is wider than the picture on 1×1 and 4×5 media.** Measured on the
  4×5 at the default window size: a **460 logical px panel against a 288px video rect**, so it
  overhangs the image on both sides and covers its lower part. The transport still reveals,
  hides and responds normally. Fixing it is aspect-ratio and window-sizing work, and the
  approved package's §8 **media-shaped window** would change the premise entirely by making the
  window adopt the source display aspect ratio.

**Three things phase 7 leaves for whatever adds UI next**:

- **The shortcut guard is Qt's, not Trace's, and it covers printable keys only.** It is now
  exercised rather than predicted — typing `hjkltefsm` into Go to Timecode puts
  `hjkltefsm` in the field and changes nothing behind it — but a new single-key shortcut still
  has to be checked against it.
- **`hasSourceTimecode_` is the single gate on everything SMPTE.** A new surface that mentions
  timecode asks it; it must not grow a second answer.
- **`OverlayHooks::holdVisible` covers popups, tooltips, modal dialogs and child focus.** The
  modal branch is live now (both Go To prompts). A new panel that takes focus inherits it.

**Run the harness the way phases 4, 5 and 6 learned to.** The clip is part of the measurement:
`transitions.ps1` needs a **16:9 clip of roughly 250+ frames** (`M&M_TopGun_1080.mp4`), because
a 9:16 clip pillarboxes four fifths of the picture signature onto black and a 121-frame clip
lets a run reach the tail inside the observation window. Both faults produce PASSes that mean
nothing. Its `-LadderOut` leg needs the **412-frame** clip and uses `FastClick`, because at 30×
that clip lasts 0.57s of wall time and `Click`'s own dwell alone spent three times the budget.

**AND MOST OF THE HARNESS NEEDS `-Env TRACE_TRANSPORT_BAR=1` NOW.** `scrub.ps1`,
`revplay.ps1`, `lifecycle.ps1`, `transitions.ps1`, `shuttleland.ps1` and `previewshot.ps1` all
locate the timeline by scanning for the docked groove's colour, and phase 6 took the bar out of
the layout. A script run without it does not fail loudly in every case — `scrub.ps1` exits at
"groove not found" **before** dragging, and the capture then reads `paints 0/1` at `frame 0`,
which looks like an app that did nothing rather than a harness that ran nothing. The overlay's
own equivalents are `overlay_drag.ps1` (drag cost, groove control built in),
`overlay_press.ps1` (the press landing) and `overlay_ladder.ps1` (the rung ladder).

**Six things phases 4–8 settled that later phases must not re-open:**

- **The Share gate's classifier is a NECESSARY condition, not a sufficient one**, and
  `lucidLinkIntegrationAvailable()` is the seam that decides Available. See the phase 9 brief
  above.
- **`assets/interface/transport/` is the approved package's glyphs plus `share`**, and
  `interface/common/` now holds exactly the three Share menu-item glyphs. `copy-lucidlink` is a
  **neutral chain glyph, not a LucidLink brand mark**; replace it only with a licensed official
  asset.

- **`landPreviousExactly` is gone and no shuttle press lands the previous run.** K, Space and
  running off the end still land. The HUD's `land N` field stays and **reads 0 through any
  press**; if a change makes it non-zero on a press, that is a regression.
- **The buttons enter both ladders at 2× and the keyboard enters at 1×.** The difference is an
  argument to `PlaybackController` (`ShuttleEntry::AtOneX`/`AtTwoX`) applied at the first rung
  only, never a call site writing `speed`. The overlay's controls trigger the same two QActions
  the bar's do, so this is inherited by construction — do not let it become a third path.
- **Artwork follows behaviour, one control at a time.** Both frame-step glyphs have left the
  tree, `interface/transport/` is exactly the approved package's glyphs, and nothing has a
  `-72` rendition. `loadIcon`'s `-72` branch is deliberately kept.
- **There is exactly one place that decides which transport is on screen**, and both
  `MainWindow` (dock the bar?) and `ViewerWidget` (draw the overlay?) ask it:
  `OverlayModel::enabledByEnvironment()`. That is what makes "no combination of knobs leaves
  the window with no transport" a property rather than a convention. A new surface asks it too.

### The rest of the phase list, with what is known about each

10. **Phase 10 is the open phase** — briefed in full above.
12. **Phase 12's Movie Inspector has most of its data already**, and `VideoMetadata` gained
    the start timecode at phase 7. Its rule is the one this project keeps proving: display
    Unknown or Untagged honestly, and do not infer missing colour metadata inside the
    inspector.
13. **Phase 13 renders the Keyboard Shortcuts window from `ShortcutTable::rows()`.** The table
    is already complete; action-owned rows point at their `QAction` rather than copying it,
    and phase 7 added `Ctrl+G` / `Ctrl+Shift+G` to it as documentation rows.

### Priority 1 is the constraint on all of it

Every phase runs the playback and scrub regression, not just the last one. `cadence.ps1`,
`scrub.ps1`, `lifecycle.ps1`, `transitions.ps1` and `stalls_vs_window.ps1` exist and the
baselines are recorded. Quote `hitch`, `win WxH` **and** `display`.

**Build the control binary in a `git worktree`, not by stashing**, and **verify every swap by
hash**. Phase 6 did it that way and the working tree was never touched; the alternative leaves
a failed `Copy-Item` running the previous binary, which silently makes an A/B two runs of the
same build. `windeployqt` the control, and copy the `av*`/`sw*` DLLs across from the main
build or it will not launch.

## Loose ends worth knowing about, none of them blocking

- **ACCESSIBILITY: THE OWNER HAS DECIDED, 2026-08-11 — the alpha ships this way and PHASE 13
  BUILDS IT.** This is a recorded decision now, not a discovery, and it should not be re-raised
  as a question.

  The facts behind it, kept because phase 13 needs them: `grep -rn
  "QAccessible\|accessibleName" src/` returns **nothing**. Trace has no accessibility code at
  all, and it never needed any while the transport was `TransportBar`'s `QPushButton`s and
  `QSlider`, because Qt exposes standard widgets to UI Automation automatically. The composited
  overlay has **no widget tree**, so it exposes nothing — and phase 6 made it the default and
  took the bar out of the layout, so the shipping build's transport went from automatically
  accessible to invisible to a screen reader.

  It is not total, which is what made the decision reasonable: every command has a keyboard
  shortcut and `ShortcutTable::rows()` enumerates them, the menus are real `QMenu`s, and
  `TRACE_TRANSPORT_BAR=1` restores real widgets. Phase 8 leaned on that deliberately — the
  Share menu's only keyboard-reachable surface is the menu bar's, and it is a real `QMenu`.

  **So phase 13 is no longer "menus, help and accessibility polish". It is the phase that
  builds an accessibility proxy tree over the overlay** (plan §19.7), and it should be
  estimated as construction rather than polish. §31.5 item 4 still stands: the overlay must
  not be called final until a screen reader has driven one.

- ~~**The keyframe grid can be learned as 2.**~~ **NO LONGER TRACKED — owner decision,
  2026-08-11.** It was one run of six on a binary that **predates phase 4**, reverse 1× is
  bimodal on that gesture anyway, and phases 5, 6 and 7 produced eighteen clean runs between
  them — all on the 1920x1080 @ 59.999Hz Parsec display rather than the panel it was seen on,
  so none of them were evidence either way. Carrying it was costing a paragraph a session and
  buying nothing. **Re-open only if reverse playback is actually reported slow**, and if it is,
  take three runs **at the panel**. (The general lesson survives the item: a single run of the
  `revplay` gesture cannot support a claim in either direction, and a clean run on the wrong
  display is not a clean run.)
- **30× is only honestly measurable on the 412-frame 1080p clip**, and even there a *held* 30×
  run traverses the whole clip in **0.57s of wall time**. That budget is smaller than the
  harness's own mouse timing: `Click` spends ~210ms of dwell per press, so six presses spanned
  ~1.6s and the ladder cap leg was capturing an **ended run**, reporting `speed 2.00x` at
  `frame 406` — which is exactly what a wrapped ladder would look like. Phase 5 added
  `FastClick` (45ms) and dropped the settle before the capture; both legs then read ±30×.
  **A leg that cannot pass is as bad as one that cannot fail, and is harder to spot, because it
  accuses the app instead of excusing it.**
- **`outside` — per-present time that is not the handler — is 3.7–15ms and unattributed**
  (`docs/reverse-shuttle-plan.md` §10 item 3). It no longer binds anything.
- **`TRACE_SCRUB_FILL_MS` ships at 60, not the 240 that §15.2's decision records.** A/B'd and
  it changes nothing measurable, so correct the note rather than the default, and re-measure
  before doing either.
- **Long-GOP slice-only threading is a closed question** — measured, refuted, knob retained as
  the control.

## The rules this project keeps re-learning

**A validated PREDICTION is not a validated MECHANISM.** Five phase records in a row stated
that `ShortcutTable`'s key-only matching made a text field dangerous, that Qt's
`ShortcutOverride` would cover it, and that it was untestable because there was nothing to
type into. All of that was right, and none of it had executed until phase 7 typed
`hjkltefsm` into a real field. Write the prediction down, but do not let it accumulate the
authority of a measurement.

**A deferred item's premise expires. Re-derive it before building it.** Seven instances now —
§26.2, §27, §28, §29.1, the BGRA cache-pricing term that had been wrong since GATE C, §15.3's
decline of directional prefetch, and phase 4's `landPreviousExactly`, whose recorded
justification cited a lease that is reclaimed unconditionally and a supersede mechanism
`dd21fe9` had already removed.

**Check what a number is measured against before believing it.** GATE E's `jitter` read 34ms
on a schedule within 1.8ms of its deadline; `stalls` read 51 on a run with 3 real hitches;
§9's "local contrast within 0.7%" concluded there was no scaling defect when there was a large
one; §29.1's "~2.3x" converted decoder throughput into a drag speed; and the phase 2 overlay's
"312 px across 12 states" was twelve captures of **one paused frame**, because the harness was
clicking 1.2px outside every control.

**A statistic over a quantity is not the quantity.** The keyframe grid was first learned as
`max(walk) + 1`, which converges *from below* and stopped at 41 on a 48-frame grid.

**A validated path is not a validated feature, and the enumeration itself goes stale.** §29.2
is the sharpest instance. Phase 3 found a seventh shuttle exit — the frame-step button — that
had been discarding a stepped frame for as long as it went unlisted, and **the obvious gesture
did not find it**: reverse → click → arrow-key passed on a broken build. It took reverse →
click → **K**. Phase 4 then found the axis itself was wrong: once a button becomes a shuttle
*entry*, "exits" no longer enumerates anything.

**And the same control on two input paths is two features.** Phase 6's overlay controls
trigger the identical QActions the docked bar's do — inherited by construction, verified, and
still wrong in one case, because *reaching* the action is not the same on both. Windows
delivers the second press of a rapid pair as `WM_LBUTTONDBLCLK`, and
`QWidget::mouseDoubleClickEvent` forwards it to `mousePressEvent` while a raw Win32 handler
does not. So the bar's ladder reached 30× on six presses and the overlay's reached 10×, from
one shared action. **The shared command hid the unshared plumbing**, and the bar could never
have shown it.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.** Two in
phase 4 alone, both silent: a 9:16 clip puts four fifths of the picture signature on black, and
an overlay aim 1.2px outside every control still prints a plausible number for all twelve
states. Phase 5 found the other kind: the ladder cap leg spent ~1.6s of mouse dwell inside a
0.57s budget, so it captured an ended run and reported a rung that looked like a wrapped
ladder. **That one accuses the app instead of excusing it**, which is why it survived a phase.
The paired discipline is the **negative control**: phase 5's re-derived matrix FAILs exactly
four cases on the phase 4 binary and passes all 25 on its own, and without that run the
re-derivation would have proved nothing. Phase 6 did the same for the double-click fault —
`overlay_ladder.ps1` reads ±30× on the fix and **±10× on a binary with the one-line fix
reverted**, which is what says the check tests the change at all.

**Phase 7's first drop-frame fixture could not have failed either, and it is the cleanest
example of the pattern.** A 29.97 clip starting at `00:59:50` and running past the hour crosses
minute 60 — a multiple of ten, where drop-frame skips nothing — so the drop and non-drop
fixtures printed **identical digits** and differed only in the separator. Moving the start to
`00:00:50` puts a *dropping* minute inside the clip and the same frame index then reads
`00:01:00;02` against `00:01:00:00`. **The fixture is part of the measurement**, exactly as the
clip is for `transitions.ps1`.

**Phase 6's ladder leg could not pass twice before it could fail once, for two unrelated
reasons, and both were arithmetic.** `restart.ps1` leaves the playhead at frame 0, so the
rewind leg ran off the head immediately and read `speed 0.00x` — a correct stop, reported as a
ladder that never climbed. Then `capture.ps1` raises the window and sleeps **300ms**, which at
30× on a 24fps clip is **216 frames**, enough on its own to run a 412-frame clip off the end
and capture an ended run. It grabs the window directly now. Phase 5 hit the second one from
the other side. **Price a harness's own dwell against the thing it is measuring before
believing its result.**

**Reproduce on the reported case AND on a healthy one before theorising.** The fast-forward
fault was reported as affecting every format. It did — but 4K H.264 still reached 3.97× of 4×
while ProRes 4444 delivered 1.00× when asked for 2×.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file",
not "a drag is in progress".

## `d3d11` is the default renderer, and two obligations follow

`TRACE_RENDERER=cpu` is the control and the escape hatch — **the first thing to try if
anything about the picture looks wrong** — but it is now the *softer* picture as well as the
slower one, since step 9 only fixed the GPU path. Say so when telling anyone to try it.

**Every scrub and playback baseline taken before 2026-08-10 was on `cpu`** and most are not
tagged with a renderer. They remain valid as records; they are **not** valid as comparisons
against a run taken today. Re-tag as you re-measure.

## Parsec — ask which display a session is on before comparing any number

Mid-session display mode changes are Anj logging in over Parsec. Remote sessions present a
virtual display at **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @ 239.999Hz**.
`scripts/measure/refresh.ps1` reports the current one.

Window size dominates cache depth and stall counts (§22.8), and resolution moves with the
refresh rate, so a Parsec run differs in two ways at once and neither shows in a bare stall
figure. And **no subjective smoothness, cadence or picture-quality judgement is valid over
Parsec** — it captures, re-encodes and re-times the screen. Every owner sign-off in this
project has been taken at the machine.

## Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either

`stalls` counts paint gaps over `2 × refresh` — 8.3ms at 239.999Hz, 33.3ms at 60Hz — so the
same run reads `stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. **`hitch` is a fixed 33ms bar
and is the only stall figure comparable across sessions.**

**`display` joined the list at phase 2.** Cache depth follows the *video rect*, not the
window, and `H` changes one without the other: `win 1280x843` with the HUD shown and hidden,
`display 640x360` against `1280x720`, `stalls 70 of 370` against `127 of 450`.

**And phase 6 showed the pair can move the OTHER way round.** Removing the docked transport
bar was expected to enlarge the video rect; at the default startup size it shrank the
**window** instead, because the window is sized from the layout's own hint and the viewer keeps
its 640×360 minimum — `win 1280x843 → 1280x767` with `display 640x360 → 640x367`, and on 4444
`display 652x367` did not move at all. That is why no stall or cache figure moved. **At a held
window size the rect really would grow**, so a maximized or user-sized window is the case to
check if a scrub number is ever questioned. The HUD names the transport too now: `+overlay`
or `+bar`.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token, so CI runs and logs can be
  read off the API — `printf 'protocol=https\nhost=github.com\n\n' | git credential fill`
  then curl with `Authorization: Bearer`. Note `git credential fill` needs the `host` field
  or it refuses.
- **CI asserts the renderer initializes**: the workflow runs `Trace.exe
  --renderer-selftest=d3d11` and fails on a fallback (exit 3), on the backend not being built
  (exit 4), or on `planar=0`. It does not `show()` and does not draw a frame, so the step-9
  reduction loop has never executed on WARP (§28.6 item 7).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop
  a running `Trace.exe` first** or the link fails with LNK1104 — and if you are swapping a
  control binary in and out, **stop it and verify the swap by hash**: a failed `Copy-Item`
  prints an error and leaves the previous binary running, which silently makes an A/B two runs
  of the same build.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles
  every `§` into mojibake. Use `cat` from the Bash tool. **A `git commit -m` here-string
  containing `>` or `->` fails with `unknown switch`** — write the message to a file and use
  `git commit -F`. **Do not name a PowerShell helper `Diff`** (`diff` is a built-in alias for
  `Compare-Object`, and aliases outrank functions) or `Move` (alias for `Move-Item`). And
  **do not pipe a measurement script through `Select-Object -First N`**: that raises
  `StopUpstreamCommandsException` and terminates the script mid-run, which looks exactly like
  the script crashing.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML; `rcc` fails at configure
  time with a parse error that names a line and not the reason.
- Harness, **and note which half needs `-Env TRACE_TRANSPORT_BAR=1` since spec phase 6**.

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1` (both directions —
  `-Forward` drives L), `transitions.ps1` (every shuttle run boundary, **25 cases**; **16:9,
  250+ frames**, and the **412-frame** clip for `-LadderOut`), `shuttleland.ps1` (the
  reverse-to-forward direction change), `scrub.ps1` (`-SnapRelease` for anything about the
  landing; `-Reversals` does not guarantee one), `lifecycle.ps1` (**run both
  `-PlayThroughDrag` and `-PausedThroughDrag`**), `previewshot.ps1`.

  *Drives the floating transport instead*: `overlay.ps1` (`-Renderer cpu|d3d11`; diff only the
  states whose frame is deterministic — and **states 06/07 are shuttle presses**, the only
  place the re-pointed hooks are actually executed), `overlay_drag.ps1` (drag cost, with the
  groove drag as its own control leg), **`overlay_press.ps1`** (does the timeline press land
  exactly — `-Bar` is the control, and both legs must be read), **`overlay_ladder.ps1`** (six
  rapid presses must reach ±30×; this is the check that caught the swallowed double-click, and
  it grabs the window directly because `capture.ps1`'s 300ms is 216 frames at 30×).

  *Mode-independent*: `cadence.ps1`, `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `sidebyside.ps1`, `stalls_vs_window.ps1`, `abfilter.ps1`/`croprect.ps1`, and
  **`make_timecode_fixtures.ps1`** — which generates the 29.97 drop/non-drop pair the asset
  set does not contain. Read its header before changing the fixture: the start time is chosen
  so a *dropping* minute falls inside the clip, and a start near a ten-minute boundary makes
  the two conventions print identical digits.

  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`), and **check nothing overlapped it**.
- Update `CLAUDE.md` and the plans at the end of the session.
