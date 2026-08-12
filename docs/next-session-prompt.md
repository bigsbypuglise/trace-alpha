# THE INTERFACE PASS IS CLOSED AND `v0.2.0-alpha.1` IS CUT. There is no open phase.

Phase 16 ran on 2026-08-11: the full regression across the whole asset set, the spec-and-rulings
audit, and both remaining owner items. **Every owner item in the pass is answered.** Record in
`docs/interface-pass-1-progress.md` under "Phase 16"; summary in `CLAUDE.md`.

**Nothing is half-finished. Do not pick up work here by inference — ask what is wanted next.**

---

## What shipped, and at what width

**`v0.2.0-alpha.1`** (`ce6e46b`, tagged 2026-08-11). Fifteen phases of interface work: the
floating transport, the bidirectional shuttle interface, fullscreen consolidation and auto-hide,
Time Display with real source timecode, the Share menu and LucidLink links, view transforms,
Open Recent, media-shaped windows, the Movie Inspector, the full menu structure with Help and
the accessibility proxy tree, and view scaling with pan.

**The minor bump was deliberate and so was staying alpha.** `v0.1.0-alpha.23` was the previous
tag and continuing that sequence would have understated an entire new interface surface. It is
**not** beta because **real mixed-monitor DPI has never executed** — §4's window-shaping system
takes `dpr` as an argument and has only ever been driven synthetically. Trace's audience runs
multi-monitor at mixed scaling, so that is the gap a wider audience hits first.
**`v0.2.0-beta.1` once a second display validates it**, and that is the obvious next step.

**The version the app reports lives in `project(Trace VERSION ...)`** in the top-level
`CMakeLists.txt` and flows through `TRACE_VERSION_STRING` into About and the Report an Issue
mail body. **Bump it with the tag** — it was 0.1.0 while the tag said 0.2.0 until this session
caught it, which would have put the wrong build in every issue report.

---

## The likely next items, in no particular order — the owner chooses

1. **Mixed-monitor DPI (§20.4), once the second display exists.** This is the named beta gate
   and the only thing standing between here and `v0.2.0-beta.1`. What has never run: a real
   `WM_DPICHANGED`, a monitor-to-monitor move, a swapchain resize across it, fullscreen on a
   secondary display. `Trace.exe --window-shape-selftest` covers the arithmetic across 11 shapes
   x 4 scale factors and **prints its own caveat on its last line** precisely so the limit
   travels with the result. **Synthetic DPR is not mixed-monitor validation and must never be
   quoted as such.**
2. **EXR and image-sequence review, with OCIO.** `TRACE_WITH_OIIO` is undefined in vcpkg and CI,
   so EXR does not open at all today. This is the largest missing *format* capability and it is
   roadmap item 7.
3. **HDR / BT.2020 tonemap.** The correct matrix is applied and there is no tonemap, so PQ/HLG
   material looks wrong on both backends. Known gap, never a complaint — but it is now named in
   public release notes, so a report may arrive. **Note §28's ordering constraint: the box
   average happens before range normalisation and the matrix because both are affine; that
   equivalence does NOT hold with a tonemap in between.**
4. **10-bit display output (step 10).** Formally deferred with **two external gates, both
   outside the code**: a confirmed 10-bit-capable display, and a defined Windows Advanced Color
   / HDR workflow. Do not conflate it with the high-bit-depth *processing* that shipped at
   GATE C.
5. **LucidLink read-ahead.** Two designs measured worse. The next experiment is stated at the
   read-ahead section of `CLAUDE.md`: satisfy FFmpeg's read callback only when the *complete*
   requested byte count is buffered, so read sizes stay ~5MB and the demuxer never repositions.
   **Benchmark before committing**, with the injected-latency knob, because a real cold cache is
   a one-shot.

**Carried as polish, not work:** the pan cursor, wanted only if it can be made trivial **and
identical on both backends** — it cannot today, because the D3D11 surface owns its own
window-class cursor and answers `WM_SETCURSOR` itself while the CPU path inherits the widget's.
**Do not describe it as a one-liner.**

**Carried as a design-package detail, not a spec item:** the temporary rate chip is top-left; the
approved package's §6 puts it centred above the transport. It was moved to top-left at phase 8
because at 84px of panel height a top-right chip overlaps a 34px control.

**Known to be OUT and staying out:** Check for Updates (no updater exists), GATE E step 2 /
vsync snapping (**stopped by owner decision — do not start it without a specific new cadence
complaint**), and §23.6 (why 4444 specifically stuttered — the fault is gone and the evidence
with it; do not re-open it speculatively).

---

## What phase 16 established that outlives it

**A NEGATIVE GREP IS ONLY EVIDENCE IF THE THING WOULD HAVE TO BE IN THAT FILE.** The handoff
into this session named one confirmed spec miss — the Movie Inspector's Duration row — quoting
the owner ruling from three documents and a verification at HEAD. **It had been built at phase
14 and was live at `MainWindow.cpp:2276` the whole time**, reading `Duration: 0:05.042 (121
frames)` with origin `encoded`, exactly as the ruling specified. The grep was aimed at
`MovieInspector.cpp`, which **by design holds no fields at all** — it takes a value type, and
every row is built in `MainWindow::buildInspectorSnapshot()`.

**Seventh stale instrument to accuse a correct build**, and the first that is a grep rather than
a measurement. The list is worth keeping whole: phase 8's menu-icon luminance, phase 9's
un-refreshed HUD after the LucidLink probe, phase 10's HUD after a view transform, phase 12's
HUD on resize, phase 14's ANSI-marshalled window titles, phase 15's HUD after a view scale, and
this. **Two of them are the same `update()`-instead-of-`repaint()` mechanism.**

**The mirror-image mistake happened in the same hour and is worth the same weight.** The
inspector's **Audio details** section looked absent from a capture; all five spec fields exist
at `MainWindow.cpp:2501-2530` and the section was simply below the fold of a 739px window. *Read
the code before concluding from a screenshot, and read the screen before concluding from a
grep.*

**ALL N CASES FAILING THE SAME WAY IS A STATEMENT ABOUT THE HARNESS'S INPUTS.** `transitions.ps1
-All` reported `groove or controls not located` on all 25 cases — the exact signature of the
phase 15 window-border fault. It was neither that nor a regression: the invocation **omitted
`-Env TRACE_TRANSPORT_BAR=1`**, which the matrix needs because it locates every control by
scanning the docked bar, and phase 6 took that bar out of the layout by default. The script's own
param block says so in six lines. **Check the invocation against the script header before
building a control binary.**

---

## The regression, as it now stands (physical panel, 5120x1440 @ 239.999Hz)

This is the baseline any future change is measured against, and it is the **first** one taken
across the whole asset set since the interface work began.

| file | cadence | `display` / `win` |
|---|---|---|
| 4K H.264 x3 | **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x | `1226x690 filtered x2` / `1226x1083` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `1226x690 filtered x2` / `1226x1083` |
| 1080p H.264 x2 | **100.0%**, 240 frames, `0 of 239` | `1226x690 filtered x1` / `1226x1083` |
| 4K 60fps x2 | **100.0%**, 162 frames, `0 of 161`, **16.67ms budget** | `1226x690 filtered x2` / `1226x1083` |
| ProRes 422 HQ x2 | **99.9%**, 168 frames, `0 of 167` | `1226x690 filtered x2` / `1226x1083` |
| 1x1 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `690x690 filtered x1` / `690x1083` |
| 4x5 ProRes x2 | **100.0%**, all gaps ~1x, `hitch 0` | `552x690 filtered x1` / `552x1083` |

`scrub -SnapRelease` `target 120 shown 120 delta 0` full-res planar, **`hitch 0`**, `land 0` ·
both lifecycle legs (83.6% and the **0% control**) · **25 of 25 transitions** · still and image
sequence both §4-shaped and zero-based · `uiatree.ps1` five named, correctly typed controls on
the drawn rects.

**`handler>budget` is not readable on the 1x1 and the 4x5** — §4 makes those windows narrow and
the dev HUD clips, which the owner ruled a **diagnostic limitation rather than a defect** at
phase 12. Bound it from what is readable rather than quoting it.

---

## Standing priorities (owner) — these outrank anything above

1. **Performance is priority #1.** No feature may ever compromise lightweight, fast, smooth
   playback. If a feature and playback smoothness conflict, the feature loses.
2. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. **Six instances**: the drag preview, §15's scrub
   sampling, accelerated reverse, accelerated forward, phase 4's shuttle-press decision, and
   phase 15's under-resolved preview at a zoom. **Do not re-open any of them on picture-quality
   grounds alone.**
3. **`V:\` is live client production storage and is strictly read-only.**

## Settled behaviour — changing any of these re-opens an owner decision

Nearest magnification above 1:1 (and therefore its point-sampled chroma) · the pan's behaviour ·
Fit to Window taking no default shortcut and staying **enabled while checked** · `kFadeMs`,
`kAutoHideMs` and the 460x84 panel with its 44/34 controls · `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context · Loop persisting across a file change and a restart · the settings home (portable
`trace.ini` beside the exe, else IniFormat under `AppConfigLocation`, **never** `NativeFormat`) ·
the §4 opening-size cap · `d3d11` as the default renderer · the 384MB reverse-cache budget ·
the accessibility proxies staying `Qt::NoFocus` and **out of the tab chain**.

## The rules this project keeps re-learning

**A validated PREDICTION is not a validated MECHANISM.** Phase 14's proxy tree had been written
down and cited for eight phases; building it exactly as described **broke the Space bar**.

**A deferred item's premise expires. Re-derive it before building it.** Nine instances.

**Check what a number is measured against before believing it.** `frames 30 | elapsed 1.25s`
read `100.0% of real time` on a looping file.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Seven times now — see phase 16 above.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**Reproduce on the reported case AND on a healthy one before theorising.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. **`gh` is NOT
  installed**, but the git credential helper holds a usable token — `git credential fill` plus
  `curl` against the REST API is how CI was polled this session.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes from
  **`docs/release-body.md`** via `body_path`, with `generate_release_notes` appending the commit
  log after it. **Rewrite that file when cutting a release, and name the known gaps plainly** —
  an alpha with honest limits gets useful bug reports.
- **CI asserts the renderer initializes** (`--renderer-selftest`, exit 3 = failed to init, exit
  4 = never built) **and the window-shape geometry across DPI** (`--window-shape-selftest`).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. Check the configure
  lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop a running `Trace.exe`
  first** or the link fails with LNK1104.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()` macros**,
  so use `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§` — use `cat` from the Bash tool. **A `git commit -m` here-string containing `>` or `->`
  fails**; write the message to a file and use `git commit -F`. **A bash heredoc containing an
  unbalanced backtick fails the same way** — write the file with the Write tool instead. **Do not
  pipe a measurement script through `Select-Object -First N`** (it raises
  `StopUpstreamCommandsException` and looks like a crash).
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @ 239.999Hz.
  **No subjective smoothness, cadence or picture judgement is valid over Parsec at all.**
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.** `stalls` is
  `2 x refresh`, so the same run reads `stalls 97 … | hitch 0`.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`, `scrub.ps1`
  (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run both legs**),
  `previewshot.ps1`. Most take no `-Clip`: **`restart.ps1` first**.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs `TRACE_NO_AUDIO=1`
  for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `viewscale.ps1`, `inspector.ps1`,
  `uiatree.ps1` (**run under `TRACE_TRANSPORT_BAR=1` as the negative control**), `phase14.ps1`,
  `menushot.ps1`, `recentfiles.ps1`, `resizecache.ps1`, `swapexe.ps1`, `banddiff.ps1`,
  `abfilter.ps1`/`croprect.ps1`, `stalls_vs_window.ps1`, `make_timecode_fixtures.ps1`,
  `make_shape_fixtures.ps1`.
- **Build a control binary in a `git worktree`, not by stashing**, and **verify every swap by
  hash** (`swapexe.ps1`). `windeployqt` the control and copy the `av*`/`sw*` DLLs across.
- Update `CLAUDE.md` and the plans at the end of the session.
