# The interface pass is open, phase 4 has shipped, and phase 5 is next.

Supersedes the previous version. **Spec phase 4 shipped** — the forward shuttle interface,
the `landPreviousExactly` decision, and a re-derived transition harness. **The next thing to
do is spec phase 5, the reverse shuttle interface**, which is now a narrow mirror of phase 4.
Paste everything below the line into a fresh session in the repo root.

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

## THE OPEN PHASE — interface pass 1, now at phase 5

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
- **Phase 4 shipped** — the forward shuttle interface. Full record in the progress doc.
- CI run 79 green on `2bb1901`, run 81 on `58bfca6`, run 84 on `cbf6d98`, all including the
  renderer selftest.

### Start at spec phase 5, and read `docs/interface-pass-1-progress.md` first

**Phase 5 is a narrow mirror of phase 4 and the shape is already proven.** Every piece of it
has a counterpart that shipped and was measured:

- `prevFrameBtn_` → `rewindBtn_`, the `rewind` glyph (which is `transport_scan_reverse`,
  already embedded and unused), a `rewindAction_` calling
  `startShuttle(-1, ShuttleEntry::AtTwoX)`, and `TransportBar::prevFrameClicked` →
  `rewindClicked`.
- `OverlayHooks::stepBack` → `rewind`, and `OverlayModel`'s left region takes the `rewind`
  glyph. The asymmetry `stepBack` / `fastForward` that phase 4 left behind is what disappears.
- `prev-frame-{24,48,72}.png` and `prev-frame.svg` leave `assets/interface/transport/` and the
  `.qrc`, in the same commit that stops the button stepping. Then `interface/transport/` is
  exactly the approved package's glyphs and nothing else.
- `prevFrameAction_` **survives**, reached only by the Left arrow. Do not delete it; the spec
  removes the button, not the command.
- `TRACE_SHUTTLE_ENTRY=2x` leaves with this phase — after it, both buttons carry the 2×
  convention as arguments and nothing needs the knob.

**Two things phase 4 settled that phase 5 must not re-open:**

- **`landPreviousExactly` is gone and no shuttle press lands.** The Rewind button passes
  nothing, because `startShuttle` takes no such parameter. Measured, both halves of the old
  justification refuted — see the phase 4 record. The HUD's `land N` field stays and **reads 0
  through any press**; if a phase-5 change makes it non-zero on a press, that is a regression.
- **`transitions.ps1` replaced `revtransitions.ps1` and its axis is a run boundary.** Phase 5
  changes what two of its cases *mean*: `R -> prevBtn` and `F -> prevBtn` stop being
  "step out of a run" and become "start a reverse run", so re-derive them rather than leaving
  them in place with the same names. Add `P -> rewBtn` and `N -> rewBtn` to `-Entries`, and add
  the reverse ladder to the `-LadderOut` leg (−2×/−5×/−10×/−30×). The `-Delayed` cases lose
  their subject entirely: there will be no frame-step button to click during a run.

**Run the harness the way phase 4 learned to.** The clip is part of the measurement:
`transitions.ps1` needs a **16:9 clip of roughly 250+ frames** (`M&M_TopGun_1080.mp4`), because
a 9:16 clip pillarboxes four fifths of the picture signature onto black and a 121-frame clip
lets a run reach the tail inside the observation window. Both faults produce PASSes that mean
nothing.

### The rest of the phase list, with what is known about each

6. **Phase 6 is the one most likely to cost performance**: removing `transportBar_` from the
   layout in favour of the floating overlay. Measure it when it lands, not at phase 14. Its
   open question is plan §31.5 item 2 — whether the overlay's timeline *press* lands exactly
   the way a groove click does. Test with the playhead deliberately far from the press point.
   Note phase 4 found the overlay harness had been aiming below its targets since phase 2; the
   aim is derived from the picture now, but **the overlay's input path has only been exercised
   properly once**, so treat its coverage as new rather than as established.
7. **Phase 7 has a decision, not just work**: the `Timecode:` readout is already synthesised
   from the frame index, which is the thing the spec forbids. Relabel it as elapsed, or
   disable it, when no source timecode exists. Phase 7 also creates **the first text-entry
   control in the app**, which is when the spec's "must not fire while focus is inside a
   text-entry control" finally has something to guard — and the shortcut table's key-only
   matching makes that check more important, not less.
10. **Phase 10 is now wiring only** — five QActions onto `viewer_->setViewTransform()`, plus
    reset on new media. `TRACE_VIEW_TRANSFORM` is the interim knob.
13. **Phase 13 renders the Keyboard Shortcuts window from `ShortcutTable::rows()`.** The table
    is already complete; action-owned rows point at their `QAction` rather than copying it.

### Priority 1 is the constraint on all of it

Every phase runs the playback and scrub regression, not just the last one. `cadence.ps1`,
`scrub.ps1`, `lifecycle.ps1`, `transitions.ps1` and `stalls_vs_window.ps1` exist and the
baselines are recorded. Quote `hitch`, `win WxH` **and** `display`.

## Loose ends worth knowing about, none of them blocking

- **The keyframe grid can be learned as 2, and snapping then engages at stride 1.** Seen on the
  phase 4 control binary (i.e. it predates phase 4): one reverse-1× run of six read
  `SNAP gop 2`, `sched tick 81ms`, **72.5% of real time**, against 100.0% and 88.1% on the two
  other runs of the same gesture on the same binary. **Reverse 1× on that gesture is bimodal**
  — `frames 114 / elapsed 4.75s` at 100%, or `frames 97 / elapsed 4.59s` at 88.1% — so a
  single run cannot support a regression claim in either direction. Take three.
- **30× is only honestly measurable on the 412-frame 1080p clip**, and even there a *held* 30×
  run traverses the whole clip in 0.57s. Phase 4's ladder leg had to press without capturing
  between presses to reach the top rung at all; capturing between them ends the run and the
  next press reads +2× **from paused**, which is correct behaviour that looks exactly like a
  ladder that wrapped.
- **`outside` — per-present time that is not the handler — is 3.7–15ms and unattributed**
  (`docs/reverse-shuttle-plan.md` §10 item 3). It no longer binds anything.
- **`TRACE_SCRUB_FILL_MS` ships at 60, not the 240 that §15.2's decision records.** A/B'd and
  it changes nothing measurable, so correct the note rather than the default, and re-measure
  before doing either.
- **Long-GOP slice-only threading is a closed question** — measured, refuted, knob retained as
  the control.

## The rules this project keeps re-learning

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

**A harness that cannot fail is not a check.** Two in phase 4 alone, both silent: a 9:16 clip
puts four fifths of the picture signature on black, and an overlay aim 1.2px outside every
control still prints a plausible number for all twelve states.

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
- Harness: `revplay.ps1` (both directions — `-Forward` drives L), `transitions.ps1` (every
  shuttle run boundary; **16:9, 250+ frames**), `shuttleland.ps1` (the reverse-to-forward
  direction change), `scrub.ps1` (`-SnapRelease` for anything about the landing; `-Reversals`
  does not guarantee one), `lifecycle.ps1` (**run both `-PlayThroughDrag` and
  `-PausedThroughDrag`**), `cadence.ps1`, `overlay.ps1` (`-Renderer cpu|d3d11`; diff only the
  states whose frame is deterministic), `playhud.ps1`, `refresh.ps1`, `capture.ps1`,
  `sidebyside.ps1`, `stalls_vs_window.ps1`, `abfilter.ps1`/`croprect.ps1`/`previewshot.ps1`.
  **Cadence controls need `TRACE_NO_AUDIO=1`**; shuttle runs do not, because they are silent.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`), and **check nothing overlapped it**.
- Update `CLAUDE.md` and the plans at the end of the session.
