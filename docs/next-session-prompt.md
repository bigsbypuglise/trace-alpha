# v0.2.0-beta.1 IS SHIPPED. THE OPEN PHASE IS THE OWNER'S UI REDESIGN ROADMAP.

## 2026-08-17, SECOND SESSION: ROADMAP STEPS 2 AND 4 ARE DONE, RE-BASELINED

Three commits (`5ff6431` · `635656a` · `c63aba4`) and one re-baseline, exactly the shape the
roadmap's cross-cutting section asked for. The shipping window is **menu bar + picture**: the
dev HUD ships hidden (`H` toggles, `TRACE_HUD=1` forces it on — `restart.ps1` passes that by
default so every harness keeps working) and the status bar is never constructed in overlay
mode. All 35 `statusBar()->showMessage` sites route through **`showTransientMessage`** — bar
mode keeps the status bar, overlay mode draws a **composited toast** on both renderers that
survives the transport's fade. The menu bar is untouched until step 7 builds its home.

**Read the session block in `CLAUDE.md` before quoting any figure** — the full re-baseline
(cadence ×7, `-SnapRelease`, reversal drag, both lifecycle legs, 25/25 transitions, §4
geometry per shape) is recorded there, taken on **1920x1080 @ 59.999Hz**, and every §4
opening size moved with the chrome. Three operational traps it records: 16:9 media now needs
`widen.ps1` after `restart.ps1` before any groove-scanning harness (the §4 window with HUD
chrome at this display is 656px wide, under the 300px groove minimum); `transitions.ps1` runs
with `-Env TRACE_TRANSPORT_BAR=1,TRACE_HUD=0`; and the D3D11 overlay quad loop's *shape* is
load-bearing — a pointer-to-ComPtr switch rewrite drew nothing while reading as equivalent,
found only by bisecting against a control build. The next roadmap step is the owner's to
choose; steps 3 (empty state), 5–7 and the rest carry their flags unchanged.

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

**Steps 2 and 4 of the roadmap change the video rect and every stall figure with it. They need
their own commit and a one-time re-baseline, done together and early — that is the next
session's first decision, not something to drift into.**

**One harness trap found and it generalises: `powershell -File script.ps1 -Env "A=1","B=2"`
flattens the array into one garbage token.** `TRACE_RENDERER` got an unknown value and fell
back to cpu, `TRACE_TRANSPORT_BAR` was swallowed, and the first "cross-backend" diff compared
cpu with cpu and read a perfect 0 — an instrument *exonerating* wrongly, which is the harder
direction to notice. Caught by reading `renderer cpu +overlay` off both HUDs. Invoke
`restart.ps1` with `&` from inside PowerShell so the array survives, and **read the HUD's
`renderer` field off both captures before believing any cross-backend diff.**

---

# Previous state (2026-08-15): v0.2.0-beta.1 shipped, no open owner decision at that time.

## 2026-08-15, SECOND SESSION: THE TWO DECK-CLEARING ITEMS ARE DONE

Both items this file listed as deferred are closed. **No new work was started, by
instruction — beta testers are on `v0.2.0-beta.1` and capacity is being kept free for
their reports.** `main` is at `3e0c936`, clean and pushed.

**(1) The package is `trace-windows-x64`** (`3b732f0`). It carries **no release stage**, so
it never has to change again — a stage word there has to move in lockstep with the three
`(beta)` literals in `MainWindow.cpp` on every promotion.

**This file said five references in the workflow. There were NINE, plus eleven more across
`README.md`, `CLAUDE.md` and three docs — twenty in the tree.** An enumeration would have
missed four in the file it named. Found by grepping, which is the rule this project already
records for `assets/` and which applied here unchanged.

**It is `DIST_NAME` at the top of the workflow now and is spelled ONCE**, so the "one typo
away from a broken publication" risk that kept this out of the release commit is removed
structurally rather than tested for. **Two `trace-alpha` strings must never be renamed and a
blind search-and-replace hits both**: the remote `bigsbypuglise/trace-alpha`, which is the
repository, and `docs/release-notes-alpha.md`, whose filename is kept so links do not break.

**Verified by a real tag build, because a branch build provably cannot.** The
`Compress-Archive` and publish steps read `skipped` in the green branch run — exactly the
two the ordinary tick cannot speak for. A throwaway `v0.2.0-beta.1-ziptest` tag published
**`trace-windows-x64.zip`, 40.8 MB**, with the ZIP step's new self-assertion reading
`release ZIP: dist\trace-windows-x64.zip (40.8 MB)`; the release and tag were then deleted
and the Releases page confirmed back to `v0.2.0-beta.1`. **Published assets were not renamed
retroactively**, so every existing release link still works.

Both builds reproduced the beta baseline to the digit: `20.8 MB` / system-DLLs-only ·
`FFmpeg detected` + `Audio dependencies detected` · `6 required files present, 95.3 MB` ·
**`renderer=d3d11 fellback=0 planar=1`** · `OK - 11 shapes x 4 scale factors`.

**(2) `verify_trace_assets.py` derives its set and is now a CI step** (`3e0c936`). Read from
`app/resources.qrc` **and `app/trace.rc`** — the second because it is a separate contract
that has already dangled once, when `assets/` was reorganised and only the `.qrc` was
re-pointed. Runs second in the workflow, before the ~20 minute build, as
`--strict --no-pillow`.

**Eleven negative controls, plus an untouched copy as the control.** The one that matters:
**a new `.qrc` entry is demanded with no edit to the script** — the only case that would
still have passed on the hard-coded version, and therefore the only one that proves the
derivation. Full detail in `CLAUDE.md`.

**One deliberate non-inclusion**: `--app-icon` covers the unembedded delivery set (macOS
renditions, extra Windows sizes, `.icns`), which has no contract file behind it. Putting
that in CI would reintroduce the hard-coded staleness the change removed. **Do not add it
without a contract to derive it from.**

**CI now has FIVE verification steps, not four** — the asset check joined them. The working
note further down still says four; read it as five.

---

Trace is **beta** as of 2026-08-15. The named gate — mixed-monitor DPI — closed on
hardware at `8945894`, which is what promoted the release: `v0.2.0-alpha.1`'s own
notes called that gate *"the main reason this release is still alpha rather than
beta"*, so cutting the beta was reading a sentence rather than making a judgement.

The 8K ProRes investigation is **closed** (owner, 2026-08-15). **Do not start stage
two, do not start CUDA/NVDEC, and do not start any new playback-architecture work.**
The next milestone is the owner's to choose; nothing here proposes one.

The working tree is clean and everything is pushed.

---

## THE RELEASE

**Tag `v0.2.0-beta.1`**, commit `066d7c9`, 36 commits since `v0.2.0-alpha.1`.
Published as a GitHub prerelease (`prerelease: true`, `draft: false`) with
`trace-alpha-windows-x64.zip`, **40.8 MB**; the body is `docs/release-body.md` and
`generate_release_notes` appends the commit log after it.

**The tag build was green and all four verification steps were read individually
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
regression figure in the release was measured on, and priority 1 says the validated
path ships. The notes name it as a knob for testers with heavy media, with the
measured +9% on the 8K plate and the reason **depth 1 is worse than off** (a depth-1
queue cannot overlap).

**Also default off and named in the notes as untested rather than as a fix:**
`TRACE_IO_READAHEAD`. It is correctness-verified and measured only against an
injected synthetic delay; **no live remote mount was available**, so its numbers are
a relative on/off comparison and must not be quoted as LucidLink performance.

### What the release contains, in the order the notes state it

Mixed-monitor DPI fixed and validated on hardware · the async scrub batch (~11x
pointer-to-picture on frame-dense cheap media, nothing sampled or skipped) · the
exact landing decoded off the UI thread (a click on the single-GOP 4K clip went
267–589ms frozen → **0ms**, with the wait unchanged and exactness untouched) · the
intra-only decode thread policy (`av_cpu_count()`, which also *helps* random access)
· the minimal LGPL FFmpeg build (20.8MB vs 104MB, ~18% faster on large ProRes) ·
the free-run-when-late scheduler fix · the bounded prefetch queue behind its knob.

### THE RELEASE STAGE IS NOT IN THE VERSION NUMBER

CMake's `VERSION` field cannot hold a prerelease suffix, so `project(Trace VERSION
0.2.0)` covers **both** the alpha and the beta of this line and the *word* is what
distinguishes them. It is a literal in `src/app/MainWindow.cpp` **three times**:
`buildIdentity()`'s `Trace %1 (beta)`, the About dialog's small print, and the
**Report an Issue mail subject**. Missing one leaves the number looking right while
the build names the wrong stage in the one place a tester will quote back.

**Verify against the built binary, not the source.** Reading the compiled strings out
of `Trace.exe` is what confirmed all three moved and none survived.

### The ZIP asset name still says `alpha`, and that is known

`trace-alpha-windows-x64.zip` is the **pipeline's artifact name**, not the stage. It
appears five times in `.github/workflows/windows-release.yml` — dist folder,
launchability check, both self-tests, artifact upload, release ZIP — and once in
`CLAUDE.md`. It was deliberately **not** renamed inside a release commit, where a typo
in any one reference breaks publication rather than a build. Stated in the release
body and in `docs/release-notes-alpha.md` so nobody reads it as the stage.

**This is the cheapest available piece of work and it is genuinely worth doing**, on
its own branch, verified by a real tag build rather than by reading. It is not
proposed as a milestone; it is a tidy-up that is currently one search-and-replace away
from being correct and one typo away from a broken publication.

---

## THE 8K LIMITATION, AS MEASURED — unchanged, and now stated in the release notes

**File:** `12_8K_ProRes4444\Foces_8K_Lut_Dino Stomp_plate_4444XQ.mov` — 7680x4320
ProRes 4444 XQ, `yuva444p12le`, 23.976 fps, 5739 Mbps.
**Machine:** AMD Ryzen 9 5900XT, 16 cores / 32 logical.

| | fps | % of 23.976 |
|---|---|---|
| Trace, minimal FFmpeg build, default threads, queue off | **13.64** | **56.9%** |
| Trace, same + stage-one queue at depth 2 | **14.87** | **62.0%** |
| Trace, shipping vcpkg build, queue off | 12.72 | 53.0% |

**Decode is the binding term and is already at its knee.** `dec 39.08ms` against a
`41.71ms` budget — **94% of the whole frame** before conversion (18.01), upload
(12.33) or paint. The curve is **flat from 32 threads to 64** and CPU never exceeds
~50% of the machine at any setting from 32 up; Amdahl on t=1/t=32 gives a 3.9% serial
fraction whose asymptote is ~26ms against a measured floor of 45ms. **The limit is
per-core throughput and memory traffic, not parallelism.** Sweep:
`docs/8k-decode-threads-sweep.md`.

**Stage two is not justified by the margin.** A *perfect* three-stage pipeline gives
`max(39.08, 18.01, ~13.5)` = **25.6 fps at zero contention** — a 6% margin over
23.976 — against contention stage one actually measured (`sws` **+24%**, `upload`
**+91%**). A 10% inflation of decode puts it below 24.

**`TRACE_RT_DROP` is an emergency fallback and must never be quoted as the 8K
result.** The owner rejected the frame-drop outcome on 2026-08-13. The acceptance bar
is unchanged and unmet: full quality, full resolution, **every frame presented**,
sustained 24000/1001, `drop 0`, `hitch 0`.

---

## PRESERVE THIS DISTINCTION: standalone decode throughput is NOT presentation throughput

Three numbers describe "how fast is 8K ProRes here", they differ by 2x, and this
project has been misled twice by conflating them.

| what is measured | figure | what it includes |
|---|---|---|
| **Standalone FFmpeg decode** (`decbench`, minimal build, t=32) | **25.24 fps** | decode only. No conversion, upload, render, seek or present. Demux subtracted. |
| **Trace's own `dec` term** (in process, minimal build) | **39.08 ms = 25.6 fps equivalent** | decode only, but inside Trace, on Trace's threads and frames. |
| **Trace presentation throughput** | **13.64 fps** | what the user sees: decode + conversion + upload + paint, strictly serial. |

**The first two agreeing within 1.5% is the useful result: Trace's decoder is not
slower than a standalone harness.** The whole gap to 13.64 is the rest of the pipeline
being serial.

**Two recorded instances of getting this wrong.** "20.5 fps, the machine cannot do it"
was the **winget `ffmpeg.exe`** — GPL/GCC, static, FFmpeg 9.0, default threads —
substituted for the LGPL/MSVC libraries Trace links. And `ffmpeg -f null` made
slice-only threading look *faster* on every ProRes file, which would have closed the
intra-only threading question as refuted: `-f null` decodes and discards, so there is
nothing for a frame-threaded decoder to overlap with. **A benchmark that removes the
work your program does around the thing being measured is measuring a different
program.**

---

## SETTLED — do not change any of these without an owner decision

- **Decode thread policy: `av_cpu_count()` clamped to 64 for intra-only; FFmpeg's
  automatic count for long-GOP.** Derived from the machine, never hard-coded — a
  four-core box must get 4 where a literal 32 would break it. Long-GOP must keep the
  automatic count because there `thread_count` is *frames in flight* and a deeper
  pipeline costs a longer refill after every seek. The HUD reads **`thr slice x32`** /
  **`thr frame x16`** so the applied count is observable rather than inferred.
  **The raised intra-only count also HELPS random access**, which is worth keeping
  because the opposite was plausible: 4K 4444 `scrub -SnapRelease` at the default
  against `TRACE_DECODE_THREADS=8` reads shuttle **29.63 → 15.89 ms/f**, `hitch`
  **2 → 0**, paints **48 → 84**, `delta 0` and full-res planar on both.
- **The async exact landing stays as it is** (`cc8e638`). `TRACE_ASYNC_LANDING=0` is
  the in-binary control and the rollback. **Rapid steps coalesce** — a stated
  behaviour change, not a side effect.
- **Stage one stays DEFAULT OFF** (`TRACE_PLAYBACK_QUEUE=0`). Turning it on by default
  is its own owner decision.
- **`TRACE_IO_READAHEAD` stays DEFAULT OFF** until validated against a real remote
  mount.
- Nearest magnification above 1:1 and its point-sampled chroma · the pan's behaviour ·
  Fit to Window taking no default shortcut and staying enabled while checked ·
  `kFadeMs`, `kAutoHideMs` and the 460x84 panel with its 44/34 controls ·
  `kMinPlaybackSpeed`, `audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and
  `frameToRgbImage`'s own swscale context · Loop persisting across a file change and a
  restart · the settings home (portable `trace.ini` beside the exe, else IniFormat
  under `AppConfigLocation`, **never** `NativeFormat`) · the §4 opening-size cap ·
  `d3d11` as the default renderer · the 384 MB reverse-cache budget · the
  accessibility proxies staying `Qt::NoFocus` and out of the tab chain.

## EXPLICITLY DEFERRED — not cancelled, not started

- **Checkpoint 2 stage two** (three-stage pipeline). Designed in
  `docs/async-decode-queue-design.md`, declined on the measurement in
  `docs/async-decode-queue-stage-one.md` and the thread sweep. Re-open only with a
  reason the 6% margin survives contention.
- **Turning stage one on by default.** Measured, safe, ~+9% on heavy media and nothing
  on media that meets budget. Needs an owner decision, not more work.
- **Hardware/GPU decode (CUDA/NVDEC).** Explicitly excluded by the owner. The only
  remaining lever that could close a 1.6x decode gap, and not to be begun.
- **The 8K plate's acceptance.** Unmet and now understood.
- **Step 10, 10-bit display output** — two external gates, unchanged.
- **LucidLink read-ahead validation against a real mount.** The third design is built
  and default off; closing it needs either a nominated file on the read-only `V:\` or
  a real cold-read figure from Anj to calibrate the synthetic knobs.
- **The 720p ComfyUI subjective comparison against QuickTime** — still not taken, and
  must be **at the machine, not over Parsec**.
- ~~Renaming the `trace-alpha-*` artifact.~~ **DONE 2026-08-15, `3b732f0`** — see the
  session block at the top.
- ~~Deriving `verify_trace_assets.py`'s set from `app/resources.qrc`.~~ **DONE 2026-08-15,
  `3e0c936`**, and it is in CI.
- ~~Mixed-monitor DPI residue.~~ **WITHDRAWN by owner decision, 2026-08-15.** The
  second display is disconnected and **no multi-display work is to be proposed.**
  §20.4's hardware pass stands — it ran at 100%/150%, found and fixed a real bug, and
  closed the beta gate; only the *remaining* list is withdrawn. It is an accepted gap,
  not an open item, and it must not reappear in a handoff. The release notes state it
  honestly as "validated at 100% and 150% only" with the untested configurations
  named, which is the right place for it. If a report ever arrives from testing, the
  hardware has to come back first and `scripts/measure/dpimove.ps1` is ready for it.

---

## `scripts/verify_trace_assets.py` — derived, and IN CI since 2026-08-15

Checks a delivered interface-icon package against the `.qrc` contract: every embedded
glyph present, every PNG exactly the pixel size its name claims, SVG masters there,
nothing extra in the working copies, and interaction-state art (`-hover`, `-pressed`)
rejected because those are a brightness multiply applied at draw time.

**`rcc` already fails the build on a missing entry, so that half is covered. A 25px
export named `-24` is not** — it builds green and renders wrong on one control at
runtime. That class is the reason it is in CI.

**The expected set is DERIVED from `app/resources.qrc` and `app/trace.rc`**, which is
what made it CI-safe: the earlier version duplicated the contract, so a `.qrc` edit
would have left it stale. Run it as `python scripts/verify_trace_assets.py` with no
arguments to check the repo against itself, or pass a staging directory to check a
delivery.

**The known soft spot is closed** — the state-suffix test strips the size first, so
`play-hover-24.png` now fails where it previously only warned.

**If you extend it, extend the derivation, not the lists.** The two hand-written sets
left in the file are the unembedded app-icon delivery convention (behind `--app-icon`,
not in CI) and the state suffixes. Anything that the build actually loads must come
from a contract file.

---

## The regression baseline (physical panel, 5120x1440 @ 239.999Hz)

| file | cadence | notes |
|---|---|---|
| 4K H.264 x3 | **100.0 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x, `drop 0`, `rephase 0` | `thr frame x16` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `thr slice x32` |
| reverse 1x | **100.0%**, 114 frames / 4.75s, `0 of 114`, `hitch 0` | |

`scrub -SnapRelease` `target 261 shown 261 delta 0` full-res planar, `hitch 0`,
`land 0` · **exact paused stepping**: `-StepCycle` landed frame 62 and ended frame 62
through 3 x (Right x5 / Left x5) · both lifecycle legs (**81.8%** and the **0%
control**) · **25 of 25 transitions** · `pq OFF 0/0 … posted 0` proving the queue
inert at its default.

Window geometry here is `win 1066x1083` (transport bar) against an older `1226x1083`;
§22.8's window-size effect applies, so compare like with like.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Thirteen
instances — and the thirteenth was written by this project's own previous session,
which is why the applied thread count is now printed in the HUD rather than described
in a note.

**Standalone throughput is not presentation throughput** (the table above).

**A validated PREDICTION is not a validated MECHANISM.**

**Check what a number is measured against before believing it.** `release` would have
read 0.1 ms on a 595 ms landing; `wait` read 52.01 ms on a run where nothing waited.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Ten times, two of which *exonerated*
one, which is harder to notice.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video
file".

**A negative grep is only evidence if the thing would have to be in that file.**

**All N cases failing the same way is a statement about the harness's inputs.**

**Reproduce on the reported case AND on a healthy one before theorising.**

**`git add` succeeding is not evidence a file was added — `git ls-files` is.** The
anchored build patterns in `.gitignore` exist because an unanchored one swallowed a
whole directory silently.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD`. **`gh` is NOT installed**; the
  git credential helper holds a usable token, and
  `printf "protocol=https\nhost=github.com\n\n" | git credential fill` will hand it to
  you for `api.github.com` calls — which is how the tag build was watched.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes
  from **`docs/release-body.md`**. Rewrite it as part of cutting a release, and **state
  the gaps as measured, with numbers**.
- **CI has FIVE verification steps and they are separate claims** (the asset check joined
  them on 2026-08-15; the four below are the originals): FFmpeg found by
  CMake (including swresample and Qt Multimedia, or audio degrades silently), the
  package is launchable, `--renderer-selftest` (exit 3 = failed to init, exit 4 = never
  built; `planar=1` asserted separately), and `--window-shape-selftest`. Read them
  individually — a green summary is not four green checks.
- Build with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. **Stop a running
  `Trace.exe` first** or the link fails with LNK1104. `build/` is vcpkg (shipping);
  `build-ffci/` is the same tree with `-DTRACE_FFMPEG_ROOT=C:/tw_ffci/out`, the minimal
  GCC FFmpeg.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()`**,
  so use `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it
  mangles every `§` — use the Write tool. **A `git commit -m` here-string containing
  `>` or `->` fails**; write the message to a file and use `git commit -F`.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @
  239.999Hz. **No subjective smoothness, cadence or picture judgement is valid over
  Parsec at all.**
- **PARK THE MOUSE CURSOR ON THE PRIMARY BEFORE ANY MEASUREMENT.** Windows launches a
  default-positioned window on the monitor the cursor is on. Quote `scr`.
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.**
- The asset set is at **`C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets`**.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`,
  `transitions.ps1` (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`,
  `scrub.ps1` (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run
  both legs**), `previewshot.ps1`, `clickland.ps1`, `stepcost.ps1`. Most take no
  `-Clip`: **`restart.ps1` first**, and **`widen.ps1` after it on portrait media** or
  the groove scan fails.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`,
  `overlay_press.ps1`, `overlay_ladder.ps1`.

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
  unavoidable, use a `git worktree` rather than stashing and **verify every swap by
  hash** (`swapexe.ps1`).
- Update `CLAUDE.md` and the plans at the end of the session.
