# After the scaling pass — the GPU initiative is done except 10-bit output

Supersedes the previous version of this file. The scrub-stall pass is closed with owner
sign-off, GPU step 8 is closed as answered-no, GPU step 9 is built and signed off, and
**step 10 (10-bit output) is the only deferred GPU item left.** No GPU item has an open
owner question. Paste everything below the line into a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. **No interface work.** `docs/interface-pass-1-spec-DEFERRED.md` is approved and
   deliberately not started. Do not begin any of it.
3. The goal for this whole phase is the core playback experience alone: smooth playback,
   locked real-time playback, responsive polished scrubbing at slow and fast speeds in both
   directions, and strong GPU integration.

**There is no owner-facing playback or scrub complaint outstanding.** Playback, the scrub
feel, and the picture have all been signed off. That is a real change in the situation: for
several sessions there was always a named complaint to chase, and now there is not. Do not
invent one — pick with the owner.

---

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first. Load-bearing sections: §9 (open
items, and note §28.1 corrects its scope), §22 (GATE C), §23 (the cadence
characterisation), §24 (GATE E), §25 (the default-renderer flip), §26 (scrub stalls), §27
(step 8, answered-no) and §28 (step 9, the scaling fix).

## The rule this project keeps re-learning — read it before picking up any deferred item

**A deferred item's premise expires. Re-derive it before building it.** Twice in two
sessions a note that was correct when written was stale when it came up:

- **§26.2** — "convert Step and cache-fill conversions to display size" was written against
  8.29MB BGRA cache entries. GATE C had already made them 3.11MB planar plane sets, so the
  remaining gain was 18% and it would have been bought by replacing a 0.25ms plane copy with
  a swscale resample. Answered no.
- **§27** — "textures are recreated on any geometry change" was true as written and was not a
  cost. GATE B's own lazy creation already reused everything: measured `tex 3` across 261
  frames of playback and `tex 4` across a 406-paint reversal drag. Answered no.

And the counter-example that makes the rule worth applying rather than a reason for
pessimism: **§28** was a deferred item whose premise had *understated* the problem, and it
turned out to be a real defect on every path in the app.

**The companion rule, now on its fourth instance: check what a number is measured against
before believing it.** GATE E's `jitter` read 34ms on a schedule within 1.8ms of its
deadline (§24.13). `stalls` read 51 on a run with 3 real hitches (§26.1). `total` silently
under-reported the shipping renderer by up to 3.47ms/frame because it never included the
upload (§27.4). And §9's "local contrast within 0.7%" concluded there was no scaling defect
when there was a large one (§28.1).

## What just happened

**The scrub-stall pass is CLOSED with owner sign-off** (§26, §26.6). The reverse-cache
budget is 384MB (was 192): 1080p `hitch 8 → 2`, 4K H.264 `hitch 3 → 1`, worst gap
169.6 → 80ms. Footprint approved, boundedness and file-change discard verified, playback
neutrality verified, and the owner confirmed the drag feels good on the shipping build.
`TRACE_REVERSE_CACHE_MB` is the control and the fallback. **Adaptive caching and convert-pool
changes were explicitly declined — do not add them off the back of this.**

**GPU step 8 is CLOSED as answered-no** (§27). See the rule above. The telemetry built to
answer it stayed, and it is worth knowing about: the HUD now reads `upload last/avg tex N`.
`tex` is cumulative `CreateTexture2D` calls since launch and is a **churn tripwire** — if a
future change to the frame path makes textures churn, that number starts climbing and says
so. The residual upload cost is memcpy bandwidth (4444's 56.6MB of planes in 3.47ms =
16.3 GB/s) and no API change reaches it; a staging buffer is strictly more work and its
justification needs the draw to be the constraint, which at `draw 0.01ms` it is not.

**GPU step 9 is DONE and SIGNED OFF** (§28, `f2d6d57`). The D3D11 sampler took one bilinear
2x2 tap, so at the shipping 6.4x downscale it read 4 source texels of every 41. Measured
against external ffmpeg references on a calibrated axis where `area` is 0 and `neighbor` is
1: **d3d11 0.74, cpu 0.73, swscale drag preview 0.76, 422 HQ 0.89.** Three unrelated
mechanisms, one number, because a 2x2 tap is a 2x2 tap. A box reduction in the pixel shader
takes 4444 to **0.02** (max channel delta vs area 46 → **2**) and 422 HQ to **0.00**, with
**no measurable playback or scrub cost** — 4444 99.8%/99.8%, 261/261, zero doubled frames,
max gap 45.3 → 44.3ms; reversal `hitch` 5,9 on against 9,7 off.

It was bigger than §9 described: **not only the Step landing, because every
full-resolution frame goes through the same sampler**, so playback undersampled too. And it
re-reads §20.3/§21.2 — CPU and D3D11 agreeing was never evidence that either was right, and
the GATE B visual sign-off was taken on that comparison. Nothing was hidden; the question
was not asked.

## Things not to undo

- **GATE E step 2 — vsync snapping and the present/decode swap — is deliberately NOT built**
  and is stopped by owner decision. Design retained unbuilt at §24.4–24.6.
  `DwmGetCompositionTimingInfo` **fails on this machine**, so there is no renderer-independent
  phase source and any future E2 is d3d11-only via `IDXGISwapChain::GetFrameStatistics`. The
  panel is **239.999Hz**, exactly 10 refreshes per 24.000fps frame. **Do not start E2 without
  a specific new cadence complaint.**
- **The drag preview's remaining softness is ACCEPTED AS-IS** (§28.6 item 2). The picture
  sharpens on release, and that is fine — previews are previews. **What the owner accepted is
  the behaviour, not a mandate to change the flag.** If it is ever wanted, the fix is
  `swsFlagsFor(fast)` returning `SWS_BILINEAR` instead of `SWS_FAST_BILINEAR` (measured
  −0.20 on the same frame), but **its cost is unmeasured and it is the dangerous kind** —
  previews are the drag path, §15.1 measured supply at 19% on 4444, and drag throughput is
  what priority #1 protects. Measure the shuttle rate and put the trade to the owner first.
- **Directional scrub prefetch stays declined** (§15.3, §26.4 item 3). Supply is 55–67% on
  the files that hitch, so the worker is saturated and has no idle time to speculate with.
- **The convert pool is still sized in pre-GATE-C currency** (§26.4 item 2) and **the owner
  declined changing it** as part of the cache work. `alloc` is 0.61–0.65ms of a 32ms frame at
  4K — visible, not binding. Needs raising with him rather than picking up.
- **Upscaling is deliberately unfiltered** (§28.6 item 5). A box average of a magnified frame
  would blur pixels someone is inspecting. The guard is `fitted < content` on both axes.

## `d3d11` is the default renderer, and two obligations follow

The owner chose it after testing both side by side (§25). `TRACE_RENDERER=cpu` is the
control and the escape hatch — **the first thing to try if anything about the picture looks
wrong** — but note it is now the *softer* picture as well as the slower one, since step 9
only fixed the GPU path. Say so when telling anyone to try it.

**Every scrub and playback baseline in the plan taken before 2026-08-10 was on `cpu`** and
most are not tagged with a renderer. They remain valid as records; they are **not** valid as
comparisons against a run taken today. Re-tag as you re-measure.

**The untested-DPI gaps are the shipping path.** Real mixed-monitor DPI has never run
(§20.4) and the box has one display.

## Parsec — ask which display a session is on before comparing any number

The mid-session display mode changes are **Anj logging in over Parsec**. Remote sessions
present a virtual display at **1920x1200 @ 60Hz**; the physical panel is **5120x1440 @
239.999Hz**. `scripts/measure/refresh.ps1` reports the current one, and this session's runs
were all on the physical panel.

Three consequences. Resolution moves with the refresh rate, so a Parsec run also has a
different window geometry, and **window size dominates cache depth and stall counts**
(§22.8) — the two effects arrive together and neither shows in a bare stall figure. 24fps is
exactly 10 refreshes at 240Hz and a 2:3 cadence at 60Hz. And **no subjective smoothness,
cadence or picture-quality judgement is valid over Parsec** — it captures, re-encodes and
re-times the screen. Owner sign-offs on feel must be taken at the machine. Note honestly
that the last two sign-offs (§26.6, §28.6) did not record which display they were taken on;
the concern was raised beforehand both times and the sign-offs were given anyway, so they
stand — but re-take at the panel before leaning on either against a future regression.

## Quote `hitch`, not `stalls`, and quote `win WxH` with either

`stalls` counts paint gaps over `2 × refresh` — 8.3ms at 239.999Hz, 33.3ms at 60Hz — so the
same 4K H.264 run reads `stalls 51 of 363 (>8.3ms) | hitch 3 (>33ms)`. **`hitch` is a fixed
33ms bar and is the only stall figure comparable across sessions.** `stalls` prints its own
threshold now.

## Candidate next work, in rough order

Nothing here is started. Pick with the owner rather than assuming — and note there is no
outstanding complaint driving any of it.

1. **4K ProRes 4444 fast drag** — still decode-bound at ~15.4ms/frame, ~2.3x playback against
   the owner's stated ~4x. Not a bug; an explicit product decision about whether to skip
   frames on the heaviest media or run the worker ahead of the request chain. 4444 gains
   least from cache work and that is structural (§26.3), so the 384MB pass did not touch it.
2. **GPU step 10 — 10-bit output.** The only deferred GPU item. Needs an `R10G10B10A2`
   swapchain and a display in 10-bit mode, and §9 warns not to conflate it with the
   high-bit-depth *processing* that shipped at GATE C. Ask whether the owner's panel and
   workflow actually want it before building it.
3. **BT.2020 has no tonemap** (§22.7 item 5). HDR/PQ content still looks wrong on both
   backends. Known gap, never a complaint. **If this is picked up, note §28.4: the shader
   averages before the matrix because both remaining steps are affine, and a tonemap between
   them breaks that — the ordering has to be revisited.**
4. **LucidLink read-ahead** — two designs measured worse; try full-request buffered serving
   before partial reads, then benchmark. Not in progress.
5. **EXR / image sequences and OCIO** (roadmap item 7). **EXR does not open today**: OIIO is
   not installed in vcpkg and not built in CI, so `TRACE_WITH_OIIO` is undefined in both.
   This is the largest untouched area of the product and the first one that is a feature
   rather than a fix — so it needs the owner's word on whether the playback phase is over.
6. **J-K-L off-speed audio, then scrub audio** (roadmap item 5). Deliberately silent today.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming. `gh` is NOT
  installed, but the git credential helper holds a usable token, so CI runs and logs can be
  read off the API — `printf 'protocol=https\nhost=github.com\n\n' | git credential fill`
  then curl with `Authorization: Bearer`. Note `git credential fill` needs the `host` field
  or it refuses.
- **CI asserts the renderer initializes** (`b5ad4d2`, `a36f10d`): the workflow runs
  `Trace.exe --renderer-selftest=d3d11` and fails on a fallback (exit 3), on the backend not
  being built (exit 4), or on `planar=0`. If you add a renderer capability that can degrade
  silently, add it to that line. **It does not `show()` and does not draw a video frame**, so
  it proves shaders *compile* and the device initializes — it has never executed the step-9
  reduction loop on WARP (§28.6 item 7).
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`. **Stop
  a running `Trace.exe` first** or the link fails with LNK1104.
- `V:\` is live client production storage and is strictly **read-only**.
- PowerShell 5.1 `Get-Content` reads as ANSI, so appending a UTF-8 doc through it mangles
  every `§` into mojibake. Use `cat` from the Bash tool for that.
- Harness: `sidebyside.ps1` (both backends at once, with a readback proving which each
  window adopted), `cadence.ps1` (cadence distribution — the only thing that can see a beat;
  presented rate cannot), `playhud.ps1` (taller crop, for `rep`/`skip` and the audio line),
  `refresh.ps1` (the display's true rational rate), `lifecycle.ps1`, `scrub.ps1`
  (`-SnapRelease` for anything about the landing, `-Reversals` does not guarantee one),
  `stalls_vs_window.ps1`. **Cadence controls need `TRACE_NO_AUDIO=1`** — 4444 has no audio
  track while 422 HQ and the 1080p clips do, so as shipped they run on different schedulers.
- **Scaling quality**, new this session: `abfilter.ps1` places a capture on a calibrated axis
  between ffmpeg `area` (0) and `neighbor` (1) references at the exact drawn size, scored by
  mean |Laplacian| — high-frequency energy is what separates aliasing from mere difference.
  `-Sensitivity` **refuses material whose two references agree**; it rejected the 4K milk
  splash and the 60fps drone plate, either of which would have passed silently. Use 4444 or
  422 HQ. `croprect.ps1` cuts the video rect from a window capture and asserts its size
  against the HUD's `display`, because a one-pixel crop error on a 6x reduction reads as a
  filtering difference. `previewshot.ps1` captures with the button still **down**, since the
  release lands a full-resolution frame. **Never use Trace as its own reference for a
  filtering question** — §20.3 spent a session on a CPU-vs-GPU difference where both sides
  were the same 2x2 tap.
- The HUD is unreadable in a downsampled screenshot on the 5120x1440 panel. Capture the
  window at native resolution (`capture.ps1`). Synthetic drags that teleport the pointer
  overstate how well the shuttle keeps up; use continuous sweeps, and keep both the smooth
  sweep and the hard-reversal gesture sets — the decode error in `2523d77` only appeared
  under reversals into both ends of the clip.
- Update `CLAUDE.md` and the plan at the end of the session.
