# The interface pass is open, phase 2 has shipped, and phase 3 is next.

Supersedes the previous version. Priority 2 was lifted on 2026-08-10 and the interface pass
was opened the same day: the spec's §2 was re-derived, both GPU prerequisites were built and
measured, the phase 1 audit was written, and **phase 2 shipped at `58bfca6`**. **The next
thing to do is spec phase 3.** Paste everything below the line into a fresh session in the
repo root.

---

## DO THIS FIRST — the asset tree was reorganised by hand and THE BUILD IS BROKEN

The owner cleaned up `assets/` on 2026-08-10, outside a session. Two directories were
deleted and the approved package's `export/` contents were moved up one level, so `assets/`
now holds `base-ui-icons/`, `player-icons/`, `png/`, `svg/`, `trace.ico`, `trace.icns` and
three `.txt` files at its root.

**`app/resources.qrc` still points at both deleted paths** — `assets/260807 Trace Media
Player Icon/export/…` (16 entries) and `assets/Interface/export/…` (6 entries). All 22 are
dangling, so `rcc` fails and the tree does not build. Verify that before anything else, then
fix it as **one commit** — the move and the `.qrc` rewrite have to land together, with a
local build and CI green, because there is no intermediate state that works.

### The target layout, as the owner specified it

```
assets/
├── branding/
│   └── app-icon/          trace.ico, trace.icns, + the png/{windows,macos} sets
│                          and svg masters the qrc embeds
├── interface/
│   ├── transport/         play, pause, rewind, fast-forward, prev-frame, next-frame
│   ├── window/            fullscreen-enter, fullscreen-exit
│   └── common/            empty — where volume/share/inspector/zoom/rotate/loop go
│                          when those features are real
├── source/
│   └── original-design-package/    the complete untouched export
└── README.md
```

`rewind` and `fast-forward` are the package's `transport_scan_reverse` /
`transport_scan_forward`. Renaming them to what they *do* is right and matches phase 2's
own rule that artwork follows behaviour.

### Three things the sketch omits, and each one is load-bearing

1. **`prev-frame` and `next-frame` must survive.** They came from the now-deleted
   `assets/Interface/`, and they are the artwork on the two *visible* side buttons, which
   still perform single-frame stepping until phases 4–5. The approved package has no
   frame-step glyph by design. Dropping them ships the scan artwork over stepping behaviour,
   which is the exact thing phase 2 refused to do. They are available at
   `assets/player-icons/{svg,png/*}/`. They leave the tree at phases 4–5, with the
   behaviour.
2. **The app embeds PNG, not SVG, and does not link `Qt6::Svg`.** An SVG-only layout would
   have nothing for the `.qrc` to reference. Carry the 1x/2x PNG renditions alongside each
   SVG master. Linking `Qt6::Svg` and moving to vector icons is a reasonable future change,
   but it is a real decision with a deployment consequence — do not let it happen as a side
   effect of a folder move.
3. **`app/resources.qrc`'s comments are project knowledge, not decoration.** They record why
   there is one icon source, why the scan glyphs are embedded-but-unused, and why the two
   step glyphs are still from the old set. Re-point the paths and keep the reasoning.

Write `assets/README.md` to say what each directory is for and state the rule that
`source/original-design-package/` is the untouched master and everything under `interface/`
is a working copy named for its behaviour.

**Regression:** icons are resources, so a build plus a visual check that the transport bar
and window icon still render is enough. Do not re-run the playback suite for this.

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
   them on picture-quality grounds alone.** Four instances now: the drag preview, §15's
   scrub sampling, accelerated reverse, and accelerated forward.

## What has been accepted, and at what width

**The core playback phase (2026-08-10)** — smooth *forward* playback, exact real-time
scheduling, responsive *bidirectional scrubbing*, and the *SDR* D3D11 GPU integration.

**The shuttle phase (2026-08-10, owner retest)** — fast-forward advancing clearly through
the complete 2×/5×/10×/30× ladder on every format; reverse 30× reading as intentional at the
approved ~15fps presentation cadence; direction changes responding correctly; stopping
landing on the last visibly displayed frame; and no regression to normal playback, audio
return, scrubbing, exact release or stepping.

**Read each at its stated width.** What was accepted in the shuttle phase is the **engine**.
A later summary that says "the transport is done" has widened it, and the widened version is
wrong — see the deferred list.

## Deferred, with the conditions attached

- **The 2×/5×/10×/30× interface.** Approved, specified, unstarted. It carries one
  requirement that is easy to lose: **the buttons must begin at 2× on their first click**,
  while the J/L keyboard convention keeps 1× as its first rung. The owner confirmed both
  readings on 2026-08-10. `startShuttleRun(direction, stride)` takes any stride, so this is
  a call site rather than engine work. Full spec in
  `docs/interface-pass-1-spec.md`; the note is inline at *Fast-forward behavior*.
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

## THE OPEN PHASE — interface pass 1, now at phase 2

The owner chose it on 2026-08-10 and lifted priority 2 to allow it. The spec is
`docs/interface-pass-1-spec.md`.

### Done on 2026-08-10 — do not redo any of it

- **The spec is renamed and open, and its §2 was RE-DERIVED** (`994dd7b`). §2 is no longer
  the 2026-08-09 text; read it as it now stands. Item 2 was stale (reverse is a call site),
  item 1 is materially larger than written (the `d3d11` default flip means the overlay needs
  a renderer-neutral home or the `cpu` escape hatch ships with no transport), item 6 gained a
  trap from step 9, item 7's mechanism was not quite as described, item 8's premise was half
  wrong, and item 9 is worse than "not extracted".
- **Both GPU prerequisites are BUILT AND MEASURED** — the renderer-neutral overlay
  (`5e1f834`) and the `VideoRenderer` view-transform contract (`4b7174f`). Plan §31 has the
  design, the measurements and the two mistakes worth keeping. Playback and scrub are
  unchanged on 4K H.264 *and* on ProRes 4444, which is the file with the least headroom.
- **The spec's phase 1 audit is `docs/interface-pass-1-audit.md`** (`7abb6a5`), twelve
  sections, read-only, each ending in what it means for the pass.
- **Phase 2 shipped at `58bfca6`** and its record is `docs/interface-pass-1-progress.md`.
  Fullscreen is a shared QAction, the dev HUD toggle is one too on **`H`**, the dead
  `showInfo`/`showTimecode`/`showSeconds` flags are deleted, `refreshHud` no longer *builds*
  a hidden HUD, and the icon tree is down to the approved `260807` package. Full regression
  against a control built from `87a39a6`, on the physical panel, all flat.
- CI run 79 green on `2bb1901`, run 81 green on `58bfca6`, both including the renderer
  selftest.

### Start at spec phase 3, and read `docs/interface-pass-1-progress.md` first

The audit's findings that change the work, in the order they bite. **Item 1 is done.**

1. ~~**Fullscreen is the only transport control that is not a shared QAction.**~~ **DONE at
   phase 2.** `fullscreenAction_`, checkable, created in `setupSharedActions()` which runs
   before `setupMenus()`. **F11 is listed first on purpose** — Qt advertises only the first
   sequence, so the order decides what the menu and the tooltip say; Ctrl+Return and
   Alt+Enter sit behind it. Escape-exits and geometry restore are still phase 6.
2. **Phase 3 should build a shortcut table rather than extend `keyPressEvent`'s flat
   switch**, because phase 13 has to render a Keyboard Shortcuts window from something.
   Phase 2 *removed* two cases from that switch (`I`, and `Return`/`Enter`) and added none —
   the two new shortcuts live on their actions. What is left in the switch is
   Space · M · Left/Right · J/K/L · F/S/T.
3. **Phases 4-5: extract the five-step shuttle sequence before adding a third caller.**
   `startShuttleRun` has exactly two callers today and each performs
   `endShuttleRun` → controller ladder → `prepareVideoRequest` → `beginPlaybackTimeline` →
   `startShuttleRun`. §29.2 is the standing warning: GATE E was validated on the Play action
   alone and every other path that started the timer compiled silently and decayed
   quadratically. And **the buttons must enter the ladder at 2×** while J/L enter at 1× —
   `jogForward`/`jogReverse` express only the keyboard contract today, so the controller
   needs a second documented way in rather than a call site poking `speed`.
4. **Phase 6 is the one most likely to cost performance**: removing `transportBar_` from the
   layout in favour of the floating overlay. Measure it when it lands, not at phase 14. Its
   open question is plan §31.5 item 2 — whether the overlay's timeline *press* lands exactly
   the way a groove click does. Test with the playhead deliberately far from the press point.
5. **Phase 7 has a decision, not just work**: the `Timecode:` readout is already synthesised
   from the frame index, which is the thing the spec forbids. Relabel it as elapsed, or
   disable it, when no source timecode exists.
6. **Phase 10 is now wiring only** — five QActions onto `viewer_->setViewTransform()`, plus
   reset on new media. `TRACE_VIEW_TRANSFORM` is the interim knob.

### Owner request, 2026-08-10 — a hotkey to hide the dev HUD — DELIVERED at phase 2

`H` toggles it, `Return`/`Enter` still work, and the dead `showInfo` was **deleted** rather
than wired up (nothing read it, so pressing `I` repainted and changed nothing; `Ctrl+I` is
the Movie Inspector at phase 12). Hiding it also stops the HUD line being *built*, which the
old `Return` binding never did — the owner's reason for wanting the key is to judge feel
without the instrument, which means without its cost. The telemetry capture still runs, so
`ra-walk` and the seek counters cannot come to mean something different depending on whether
the HUD happened to be visible.

**The measurement consequence was real and THIS NOTE NAMED THE WRONG GUARD.** It said the
existing discipline covers it "because `win WxH` changes with the toggle". It does not
change. Measured on one 4K H.264 reversal drag, HUD shown against hidden: `win 1280x843`
**both times** — the window does not resize, the *viewer* takes the HUD's height. What moves
is the video rect, which the HUD reports as `display`: **640x360 → 1280x720**, and with it
`stalls 70 of 370 → 127 of 450`. **Quote `display` as well as `win WxH` for any number taken
with the HUD toggled.** `hitch` read **1 either way**.

### Priority 1 is the constraint on all of it

Every phase runs the playback and scrub regression, not just the last one. `cadence.ps1`,
`scrub.ps1`, `lifecycle.ps1` and `stalls_vs_window.ps1` exist and the baselines are recorded.
Quote `hitch` and `win WxH`. The overlay and the auto-hide animation are the two items most
likely to cost something, and they are early — measure them when they land, not at phase 14.

## Loose ends worth knowing about, none of them blocking

- **30× is only honestly measurable on the 412-frame 1080p clip.** Every other test file
  traverses in under 0.2s at that speed, so those cells read "clip-limited". A longer clip
  would make that row trustworthy on all four formats.
- **`outside` — per-present time that is not the handler — is 3.7–15ms and unattributed**
  (`docs/reverse-shuttle-plan.md` §10 item 3). It no longer binds anything: the shuttle holds
  presentation at 24/s, or 15/s snapped, at every speed. It would matter again to any future
  work that wants a higher presentation rate.
- **`TRACE_SCRUB_FILL_MS` ships at 60, not the 240 that §15.2's decision records.** A/B'd on
  4K H.264 backward drag and it changes nothing measurable, so this is a
  documentation-versus-code discrepancy rather than a regression to chase. Correct the note
  rather than the default, and re-measure before doing either.
- **Long-GOP slice-only threading is a closed question** — measured, refuted, and the knob
  (`TRACE_LONGGOP_SLICE_THREADS=1`) is retained as the control. Frame threading is what makes
  the GOP walk cheap.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Six instances now —
§26.2, §27, §28, §29.1, the BGRA cache-pricing term that had been wrong since GATE C, and
§15.3's decline of directional prefetch, which was correct for the drag and the opposite of
correct for a fixed-rate reverse run.

**Check what a number is measured against before believing it.** GATE E's `jitter` read 34ms
on a schedule within 1.8ms of its deadline; `stalls` read 51 on a run with 3 real hitches;
`total` under-reported by 3.47ms/frame; §9's "local contrast within 0.7%" concluded there was
no scaling defect when there was a large one; §29.1's "~2.3x" converted decoder throughput
into a drag speed.

**A statistic over a quantity is not the quantity.** The keyframe grid was first learned as
`max(walk) + 1`, which converges *from below* and stopped at 41 on a 48-frame grid — so every
"snapped" target missed and still walked, while the HUD read `SNAP gop 41` and nothing
improved. The keyframe *positions* were exact and available all along.

**A validated path is not a validated feature.** §29.2 is the sharpest instance: GATE E was
validated on the Play action alone and every other path that started the timer kept compiling
silently. Enumerate the entry points rather than testing the one the harness drives —
`scripts/measure/revtransitions.ps1` exists for exactly this and covers all six shuttle exits.

**Reproduce on the reported case AND on a healthy one before theorising.** The fast-forward
fault was reported as affecting every format. It did — but 4K H.264 still reached 3.97× of 4×
while ProRes 4444 delivered 1.00× when asked for 2×. Measuring both is what showed it was one
shared fault with a per-format threshold rather than four bugs.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file",
not "a drag is in progress". Guarding the shuttle on it disabled the entire pipeline while
every counter looked healthy, with `posted 0` on the worker line as the only symptom.

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
project has been taken at the machine, including the shuttle one.

## Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either

`stalls` counts paint gaps over `2 × refresh` — 8.3ms at 239.999Hz, 33.3ms at 60Hz — so the
same run reads `stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. **`hitch` is a fixed 33ms bar
and is the only stall figure comparable across sessions.**

**`display` joined the list at phase 2.** Cache depth follows the *video rect*, not the
window, and `H` now changes one without the other: `win 1280x843` with the HUD shown and
hidden, `display 640x360` against `1280x720`, `stalls 70 of 370` against `127 of 450`. A bare
`win WxH` does not disambiguate a run taken with the HUD off.

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
  a running `Trace.exe` first** or the link fails with LNK1104.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles
  every `§` into mojibake. Use `cat` from the Bash tool. **And a `git commit -m` here-string
  containing `>` or `->` fails with `unknown switch`** — write the message to a file and use
  `git commit -F`. **Do not name a PowerShell helper `Diff`**: `diff` is a built-in alias for
  `Compare-Object` and aliases outrank functions, so the helper is never called.
- Harness: `revplay.ps1` (both directions — `-Forward` drives L), `revtransitions.ps1` (all
  six shuttle exits), `scrub.ps1` (`-SnapRelease` for anything about the landing;
  `-Reversals` does not guarantee one), `lifecycle.ps1` (**run both `-PlayThroughDrag` and
  `-PausedThroughDrag`**), `cadence.ps1`, `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `sidebyside.ps1`, `stalls_vs_window.ps1`, `abfilter.ps1`/`croprect.ps1`/`previewshot.ps1`.
  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`), and **check nothing overlapped it** — a
  foreground window spoiled one capture in this session.
- Update `CLAUDE.md` and the plans at the end of the session.
