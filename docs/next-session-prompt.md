# Prompt for the next Claude Code session — step 8, with two things first

Supersedes the previous version of this file (step 6.5 / GATE C, both now done at `05dca49`).
Paste everything below the line into a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. **No interface work.** `docs/interface-pass-1-spec-DEFERRED.md` is approved and
   deliberately not started. Do not begin any of it.
3. The goal for this whole phase is the core playback experience alone: smooth playback,
   locked real-time playback, responsive polished scrubbing at slow and fast speeds in both
   directions, and strong GPU integration.

---

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first. We are at `05dca49`. GATE C landed
at `e8566a4`.

**GATE B is PASSED — owner visual sign-off, 2026-08-09.** CPU and D3D11 are visually
equivalent in fit-window and fullscreen; the 150% result is acceptable with no meaningful
softness, scaling artifacts, colour difference or framing difference. Verdict: proceed with
D3D11. Record this in plan §17.5 item 2 and §20.2, which is the last thing either was waiting
on. ProRes 4444 **scrub** also passed. `cpu` remains the default renderer until GATE E.

**New owner report, and it is the reason the order below changed:** ProRes 4444 **playback**
is not locked to real time — slight, perceptible stutter. That is now the highest-value open
question in the project, because "locked real-time playback" is priority #1 and this is the
first direct report against it.

Four items, in this order. The first two are short, the third is measurement only.

## 0. Housekeeping — the working tree is lying

`git status` shows 24 modified source files with **11304 insertions and 11304 deletions**.
That is every line in every file: pure CRLF/LF churn, not real changes. It has been present
across at least three sessions and it makes "clean git status" meaningless as a final-report
item — worse, it invites someone to commit 11k lines of line-ending noise on top of real work.

Fix it properly: add a `.gitattributes` that pins the text handling for `.cpp/.h/.hlsl/.md/
.ps1/.txt/.yml/.cmake` and `CMakeLists.txt`, confirm `core.autocrlf` agrees, renormalise once
in its own commit, and verify `git status` comes back clean.

Then commit the untracked material that should be in the repo:

- `assets/260807 Trace Media Player Icon/` — 201 approved interface assets (icons only; the
  interface pass itself stays deferred)
- `docs/interface-pass-1-spec-DEFERRED.md`
- `docs/next-session-prompt.md`

`docs/next-session-brief.md` is superseded by this file — delete it. `.claude/` should
probably be ignored rather than committed; check whether it holds anything shared.

## 1. Settle the 4K H.264 stall number before it becomes folklore

~44 stalls of ~375 on reversals, against `2 of 394` recorded at §17.4 on the same file and
gesture. It reads the same on **both renderers** and on a **control build of the preceding
commit**, so it is not this session's work.

**Check the commit range before assuming it is environmental.** Between `8a7cdb3` (where
§17.4 was measured) and the control build there are only docs and chore commits — no code
changed. If that holds, the code cannot be the cause and the difference is in the
*measurement conditions*. Confirm that first; it is a two-minute `git log --stat` and it
decides whether this is a bug hunt or a harness problem.

**The prime suspect is window size, and it is specific.** The frame cache is budgeted in
**bytes** (`b5a56af`), and scrub previews convert to **the size the viewer will draw them
at**. So a larger window means larger previews, which means *fewer cache entries fit the same
byte budget*, which means a lower hit rate, which means more seek-and-GOP-walk stalls. Cache
depth is a function of window size. If §17.4 was captured at a different window size than
this session's runs, a 20x stall difference is fully explained and there is no regression.

Test it directly: re-run the §17.4 reversal gesture at two or three window sizes on 4K H.264
and report stalls, `rev-hit %` and the derived cache `cap` for each. Second suspect is warm
vs cold file cache; third is machine load. Whatever the answer, **record the window size in
the harness output from now on** — a stall count without it is not comparable across
sessions, which is the actual defect here.

Do not spend more than an afternoon. If it is a real regression, stop and say so; if it is a
measurement-conditions artifact, write that into the plan next to the §17.4 numbers so nobody
re-opens it.

## 2. Characterise the ProRes 4444 playback stutter — MEASUREMENT ONLY, no fixes

Do not write a fix in this item. The project has three reverted scheduler experiments on
record precisely because the measurement came second. The deliverable here is a number that
says which of two unrelated causes dominates, and they need different work.

### The two candidates, and why the average rate cannot tell them apart

**Cause A — the structural beat from the integer tick.** The playback tick is
`floor(1000/fps)` = **41ms** against a **41.667ms** frame at 24fps. With no audio track — and
4444 has been the no-audio control throughout — the wall-clock accumulator gates presentation
and carries its residue forward, so the *average* rate is right. But the accumulator falls
0.667ms short each frame, and roughly every **62 frames** it needs two ticks to clear the
threshold, producing a single **82ms** frame in a 41.667ms stream. That predicts **about one
doubled frame every 2.6 seconds**, which matches the `rep 4–5 per 10s` already in the record.
An 82ms frame is very visible. **This is not 4444-specific** — it is on every file, it is a
presentation-clock problem, and GATE E is what fixes it.

**Cause B — per-frame cost variance specific to 4444.** Playback decodes synchronously on the
UI thread with no read-ahead, so a frame that takes longer than the budget is late
immediately; there is nothing absorbing it. 4444's handler is 25.22ms of a 41.67ms budget
after GATE C, the healthiest it has ever been *on average* — but the average is not the
question. Decode alone is ~15.4ms and ProRes has no `lowres` path. **GATE E does not fix
this**: a presentation clock supplies phase, not headroom. If a frame is not ready, no
scheduler helps.

The rate metric (frames presented ÷ real time) reads 98.3–99.6% under **both** causes,
because it averages. That is why 4444 can measure 99% and still stutter, and it is why this
item exists.

### What to measure

On 4K ProRes 4444, a full-clip playback run, several times:

1. **The distribution of intervals between consecutive presents** — not the mean. Report the
   histogram or at least p50/p95/p99/max. Cause A predicts a tight cluster at ~41ms with a
   sparse, *regular* population near 82ms. Cause B predicts a ragged spread with irregular
   long tails. The shapes are different enough to read off a histogram.
2. **Per-frame handler time distribution**, and the count of frames whose handler exceeded
   41.667ms. Cause B's signature is that this count is non-zero.
3. `rep` / `skip` / drift / presented rate, for continuity with the record.
4. Confirm the file's exact rate from the stored rational (`7b924be`) and `TRACE_OPEN_LOG=1`
   — 24.000 and 23.976 give different beat periods and it matters which one this is.
5. **The same run on a file the owner has signed off as smooth** (1080p H.264, and 4K ProRes
   422 HQ). Cause A should appear at the same rate on all three; anything extra on 4444 is
   cause B. Without this control the measurement cannot separate them at all.
6. Both renderers, and `TRACE_PLANAR_UPLOAD` on and off. GATE C moved 4444's handler
   35.2 → 25.2ms, so if the stutter predates GATE C it should be *better* now — establish
   whether it is.

### Then say which it is, and stop

Report the split. If it is mostly cause A, that is an argument to **pull GATE E forward ahead
of items 9 and 10** — say so explicitly and let the owner decide; do not reorder the plan
unilaterally. If cause B contributes materially, items 8 and 9 are the work that buys the
headroom GATE E will need on the heaviest file anyway, and the order below is already right.

Commit as `docs: characterise ProRes 4444 playback cadence` plus whatever harness additions
it needed.

## 3. Step 8 — texture and upload-resource reuse

Plan §8 item 8. Take it after the measurement above, and take it for a specific reason rather
than because it is next in the list: **the one unexplained result from GATE C is the 4444
release latency going 6 → 33ms reproducibly, while every per-frame cost it is made of
improved** — and 4444 is now also the file with the playback complaint. Step 8 touches exactly
the machinery both would live in. If item 2 finds cause B contributing, widen this step's
motivating measurement from release latency to **4444 per-frame cost variance during
playback**; they are the same upload path.

### Run the control first — the knob already exists

`TRACE_PLANAR_UPLOAD=0` restores swscale BGRA on the d3d11 path. Measure 4444 release latency
with it on and off, three runs each. If latency returns to ~6ms with planar off, the landing
path is the cause and the rest of this step has a target. If it does not, the cause is
elsewhere and step 8 should not be expected to fix it — say so rather than shipping the
optimization and claiming the number.

### Where to look

`D3D11VideoRenderer` keeps **one** cached BGRA texture (`ensureTexture`) and **one** cached
plane-texture set (`ensurePlaneTextures`), each keyed on size and format, each destroying and
recreating the whole set on any mismatch. A drag on 4444 alternates two shapes through that
machinery: **previews are BGRA at display size**, and **the landing is planar at full res**
(3 planes, 16-bit, ~56.6MB). Candidates, in the order they are cheap to test:

1. Plane textures being recreated per landing rather than reused across landings.
2. `D3D11_MAP_WRITE_DISCARD` on a ~56MB DYNAMIC resource — WRITE_DISCARD asks the driver for
   fresh backing on every map, which is nearly free on a small BGRA texture and is not on
   this. A staging resource plus `CopyResource`, or `MAP_WRITE_NO_OVERWRITE` with explicit
   fencing, is the usual answer; measure before choosing.
3. The convert pool still allocating a full-res planar buffer per landing. Note the pool
   thrash fixed in `e8566a4` was this class of bug and the lesson generalises: **a policy that
   is a no-op under one buffer kind can become a thrash under two, and nothing about the first
   workload predicts it.** Two kinds now flow through this path.

Instrument the release into its component terms — decode, conversion-or-passthrough, buffer
acquire, texture ensure, map + memcpy, draw + present — before changing anything. "Release
latency" as one number cannot say which term grew, and that is why it is currently unexplained.

### Scope discipline

Resource reuse only. Do not fold in GPU scaling — that is item 9 and it changes what the
picture is, not just how it is uploaded. Keep the BGRA path working; it is the control for
every planar measurement.

### Validation

- 4444 release latency, three runs, against the GATE C figures (6 → 33ms) and the
  `TRACE_PLANAR_UPLOAD=0` control.
- Playback on all seven files, both renderers, against the recorded controls (98.3–99.6%
  presented). **Do not expect a rate win** — GATE C already established none of these files
  is conversion-bound at 24fps, and step 8 buys headroom, not rate. Book it honestly.
- Scrub unchanged: decoder throughput, `rev-hit %`, stalls, with **window size recorded**.
- `delta 0`, `detach 0.00`, `stale-blocked 0`, `recov 0` on every row.
- `scripts/measure/lifecycle.ps1` in full, including `-PlayThroughDrag` and its
  `-PausedThroughDrag` control.
- The same-renderer control harness from this session reads exactly 0 and is the noise floor
  for any A/B. Use it.

Commit as `perf(gpu): reuse textures and upload resources`.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming — the
  `~/Claude/Trace` path is macOS-only and handing it to Anj here fails silently.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`.
- `V:\` is live client production storage and is strictly read-only.
- Update `CLAUDE.md` and the plan at the end of the session.
