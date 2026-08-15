# OPEN: ONE OWNER DECISION — stage two was funded on a margin that stage one has now measured away

Both of the previous session's decisions were answered and **all three items in the stated order
are built, measured and committed**: the exact landing off the UI thread (`cc8e638`), the
scrub-versus-playback confirmation (`94e0c13`), and checkpoint 2 stage one (`d8beba8`). Stage two
is **not started, by instruction** — the owner asked to be reported to first, and the report
changes the premise stage two rests on.

## THE DECISION

**Stage two was funded to take the 8K plate close to real time. Stage one measured the margin it
depends on and the margin is not there.**

Full report: **`docs/async-decode-queue-stage-one.md`**.

Stage one works and is real. Overlap is confirmed on the design's own terms rather than on the
frame rate — `handler` collapses 70.42 → 16.28ms while `dec` stays 38.55 → 41.08 and `outside`
rises 3.13 → 28.32. But it is **+9.7% relative, not the +22% it was scoped at**, and the reason is
the whole of this decision: **both overlapped stages get slower when run concurrently.** `sws`
+24%, `upload` +91%, on a 199MB frame. The design flagged contention as a *stage-two* risk against
a **~2ms per frame** margin; it is **already material at one stage**.

| 8K plate, `TRACE_RT_DROP=0` | off | d1 | d2 | d3 |
|---|---|---|---|---|
| vcpkg | **53.6%** | 48.3% | **58.2%** | clamps to 2 |
| minimal GCC FFmpeg | **56.5%** | — | **62.0%** | — |

Stage two would split conversion off the worker onto a third thread, taking the worker's stage to
`dec` alone — **41.08ms = 24.3 fps on the minimal build, nominally at the 23.976 target with no
margin at all**, before that third thread's own contention, which stage one has just measured at
+24% and +91% on the two terms it would touch. On the numbers in hand **stage two lands short of
24 rather than at it.**

**The three options, and the third is not in the funded plan and is what the numbers point at:**

1. **Stop after stage one.** Ship the queue at depth 2 (default-on is its own decision), take the
   ~10%, accept the plate at ~62%.
2. **Build stage two anyway**, knowing the prediction now has measured contention in it.
3. **Attack decode.** `dec` is the binding term in *every* arrangement and is the one thing
   neither stage touches. **`TRACE_DECODE_THREADS` is already known to be worth +21% on this
   plate** and still sits at FFmpeg's automatic count, **which caps at 16 on a 32-thread box**.
   Cheapest of the three. Recorded as a recommendation, not taken.

**The 8K acceptance is unchanged and the file is NOT signed off**: full quality, full resolution,
every frame presented, sustained 24000/1001, `drop 0`, `hitch 0`, exact final frame, bounded
memory, regression unchanged. **`TRACE_RT_DROP` is an emergency comparison only. Do not begin CUDA
work.**

---

## What shipped this session

### The exact landing is off the UI thread (`cc8e638`)

The fix for decision 1. **Exactness is untouched** — `RequestMode::Step`, one frame, full
resolution, accurate conversion, `batch 1`, `batchBudgetMs` **deliberately 0** because a budget on
one frame could only mean "give up". Only the thread changed.

Seedance clip, **`TRACE_ASYNC_LANDING=0` as the in-binary control**:

| gesture | control | async |
|---|---|---|
| click at 30/60/85% | **267/424/589 ms frozen** | **0/0/0 ms** |
| backward step x16 | max **410** avg **29 ms** | max **31** avg **14 ms** |
| forward drag `ui gap max` | **227.7 ms** | **8.3 ms** |
| `ui over 16ms` | 1 of 1139 | **0 of 1516** |
| `release` (time to picture) | 597.7 ms | 595.2 ms |

**The last row is the point: the walk is the same walk.** This makes 520ms not a freeze; it does
not make it shorter.

**RAPID STEPS NOW COALESCE and that is a stated behaviour change, not a side effect.** Five fast
presses move five frames and decode the fifth. The arithmetic is identical, `+5` then `-5` returns
to the same frame (`lifecycle -StepCycle`: landed 62, ended 62, with `landing async 32 sync 1`
showing all 30 steps landing individually at 120ms spacing), and the frame the user stops on is
exact. What it replaces is worse by every measure. **If the owner dislikes it, `TRACE_ASYNC_LANDING=0`
is the revert and it takes the freeze back with it.**

Three faults the first cut had, all in `CLAUDE.md`: a click decoded the same frame **twice** until
`requestExactFrameAsync` learned to **adopt** an in-flight landing; a mid-drag flush **re-froze the
window** because `activeScrubFrame_` is still -1 while the press landing walks; and **`release`
would have read 0.1ms and flattered the build**.

### Why the 8K plate scrubs better than it plays (`94e0c13`)

`docs/8k-scrub-vs-playback.md`. Two of the four predicted reasons needed correcting, and **the
decisive one was not on the list: a drag has no deadline and playback does.** The decoder supplies
the same **~13 fps** either way. Playback is judged against 23.976 and misses on *every* frame;
a drag is judged against the pointer, which either asked for 13.8 f/s and was met, or asked for
163.9 f/s and was legitimately allowed to trail and sample. **Scrubbing feels better because
nothing promised it 24.** What the pipeline has to buy back is **13 → 24 fps at full resolution**.

---

## Regression baseline (physical panel, 5120x1440 @ 239.999Hz) — re-taken this session

| file | cadence | `display` / `win` |
|---|---|---|
| 4K H.264 x3 | **99.9 / 100.0 / 100.0%**, 120 frames, `0 of 119`, all gaps ~1x, `drop 0`, `rephase 0` | `1066x600 filtered x2` / `1066x1083` |
| ProRes 4444 x2 | **99.8%**, 261 frames, `0 of 260` | `1066x600 filtered x2` / `1066x1083` |
| reverse 1x | **100.0%**, 114 frames / 4.75s, `0 of 114`, `hitch 0` | — |

`scrub -SnapRelease` `target 120 shown 120 delta 0` and `target 261 shown 261 delta 0`, both
full-res planar, **`hitch 0`**, `land 0` · both lifecycle legs (81.8% and the **0% control**) plus
all seven other legs · **25 of 25 transitions** · `pq OFF 0/0 … posted 0` proving the queue inert
at its default.

**Window geometry differs from the previous baseline** (`1066x1083` against `1226x1083`) because
these runs were taken with the transport bar; §22.8's window-size effect applies, so compare like
with like before calling anything a regression.

`revplay -StepCheck` reports **`+1 moved 0%`** on 4K H.264 and **the control reads the same** — the
reverse run lands on a static opening frame. Not attributable to any change here; pick a different
position if that leg is ever needed as a real control.

---

## Standing priorities (owner) — these outrank anything above

1. **Performance is priority #1.** No feature may ever compromise lightweight, fast, smooth
   playback.
2. **Smooth, responsive motion beats matching final-frame fidelity during motion.** Fidelity is
   owed to the frame the user stops on. **Six instances**, and **decision 1 of the last session
   deliberately did NOT add a seventh** — a click is a landing, and the owner's ruling was that
   exactness is not what costs anything there. **Do not weaken the landing.**
3. **`V:\` is live client production storage and is strictly read-only.**

## Settled behaviour — changing any of these re-opens an owner decision

Nearest magnification above 1:1 (and its point-sampled chroma) · the pan's behaviour · Fit to
Window taking no default shortcut and staying **enabled while checked** · `kFadeMs`,
`kAutoHideMs` and the 460x84 panel with its 44/34 controls · `kMinPlaybackSpeed`,
`audioShouldDrive()`'s `== 1.0`, the three loop-wrap sites and `frameToRgbImage`'s own swscale
context · Loop persisting across a file change and a restart · the settings home (portable
`trace.ini` beside the exe, else IniFormat under `AppConfigLocation`, **never** `NativeFormat`) ·
the §4 opening-size cap · `d3d11` as the default renderer · the 384 MB reverse-cache budget ·
the accessibility proxies staying `Qt::NoFocus` and **out of the tab chain**.

## The rules this project keeps re-learning

**A deferred item's premise expires. Re-derive it before building it.** Twelve instances.

**A validated PREDICTION is not a validated MECHANISM.**

**Check what a number is measured against before believing it.** `release` would have read 0.1ms
on a 595ms landing, and `wait` read 52.01ms on a run where nothing waited — both this session.

**A harness that cannot fail is not a check — and one that cannot PASS is worse.**

**An instrument can accuse a correct build.** Nine times now — and two of those *exonerated* one,
which is harder to notice.

**Names lie; read the definition.** `isVideoScrubActive()` means "the media is a video file".

**A negative grep is only evidence if the thing would have to be in that file.**

**All N cases failing the same way is a statement about the harness's inputs.**

**Reproduce on the reported case AND on a healthy one before theorising.**

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. **`gh` is NOT
  installed**; the git credential helper holds a usable token.
- **A `v*` tag publishes a real ZIP and marks the release prerelease.** The body comes from
  **`docs/release-body.md`**.
- **CI asserts the renderer initializes** (`--renderer-selftest`, exit 3 = failed to init, exit
  4 = never built) **and the window-shape geometry across DPI** (`--window-shape-selftest`).
- Build with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md`. **Stop a running `Trace.exe`
  first** or the link fails with LNK1104. `build/` is vcpkg; **`build-ffci/` is the same tree with
  `-DTRACE_FFMPEG_ROOT=C:/tw_ffci/out`** and is the minimal GCC FFmpeg — both build clean and both
  were measured this session.
- **`windows.h` arrives through the D3D11 backend's header and defines `max()`/`min()`**, so use
  `qMax`/`qMin` in `src/render/VideoRenderer.cpp`.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles every
  `§` — use the Write tool. **A `git commit -m` here-string containing `>` or `->` fails**; write
  the message to a file and use `git commit -F`.
- **XML comments cannot contain `--`.** `app/resources.qrc` is XML.
- **Run `scripts/measure/refresh.ps1` at the start of a session and again before quoting
  anything.** Parsec presents 1920x1200 @ 60Hz; the physical panel is 5120x1440 @ 239.999Hz.
  **No subjective smoothness, cadence or picture judgement is valid over Parsec at all.**
- **PARK THE MOUSE CURSOR ON THE PRIMARY BEFORE ANY MEASUREMENT.** Windows launches a
  default-positioned window on the monitor the cursor is on. Quote the HUD's `scr` field.
- **Quote `hitch`, not `stalls`, and quote `win WxH` AND `display` with either.**
- The asset set is at **`C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets`**.
- Harness, and **which half needs `-Env TRACE_TRANSPORT_BAR=1`**:

  *Needs the docked bar* (they scan for its groove colour): `revplay.ps1`, `transitions.ps1`
  (**16:9, 250+ frames — `M&M_TopGun_1080.mp4`**), `shuttleland.ps1`, `scrub.ps1`
  (`-SnapRelease` for anything about the landing), `lifecycle.ps1` (**run both legs**),
  `previewshot.ps1`, `clickland.ps1`, `stepcost.ps1`. Most take no `-Clip`: **`restart.ps1`
  first**, and **`widen.ps1` after it on portrait media** or the groove scan fails.

  *Drives the floating transport*: `overlay.ps1`, `overlay_drag.ps1`, `overlay_press.ps1`,
  `overlay_ladder.ps1`.

  *Mode-independent*: `cadence.ps1` (**scratch `TRACE_SETTINGS_FILE`; needs `TRACE_NO_AUDIO=1`
  for controls**), `playhud.ps1`, `refresh.ps1`, `capture.ps1`, `widen.ps1`, `viewscale.ps1`,
  `inspector.ps1`, `uiatree.ps1`, `phase14.ps1`, `menushot.ps1`, `recentfiles.ps1`,
  `resizecache.ps1`, `swapexe.ps1`, `banddiff.ps1`, `abfilter.ps1`/`croprect.ps1`.

  *Needs two displays at different scale factors*: **`dpimove.ps1`**.
- **Do not run `transitions.ps1` on a 9:16 clip.** Its own header records that pillarboxing
  produces PASSes that mean nothing.
- **Build a control binary in a `git worktree`, not by stashing**, and **verify every swap by
  hash** (`swapexe.ps1`) — though note both changes this session shipped with an **in-binary**
  control, which is better: the two runs differ in one branch rather than in a compile.
- Update `CLAUDE.md` and the plans at the end of the session.
