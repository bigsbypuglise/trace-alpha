# Prompt for the next Claude Code session — the GATE E decision, then step 8

Supersedes the previous version (housekeeping / stall number / 4444 cadence, all now
done at `2ddbe8b`). Paste everything below the line into a fresh session in the repo root.

---

## Standing priorities (owner) — these outrank anything below

1. **Performance is priority #1.** No interface feature may ever compromise lightweight,
   fast, smooth playback. If a feature and playback smoothness conflict, the feature loses.
2. **No interface work.** `docs/interface-pass-1-spec-DEFERRED.md` is approved and
   deliberately not started. The icon assets are committed; that is storage, not permission.
3. The goal for this whole phase is the core playback experience alone: smooth playback,
   locked real-time playback, responsive polished scrubbing at slow and fast speeds in both
   directions, and strong GPU integration.

---

Read `CLAUDE.md` and `docs/gpu-initiative-plan.md` first. We are at `2ddbe8b`.

**GATE B is PASSED** (owner sign-off, recorded in plan §20.2 and §17.5 item 2).
**GATE C is implemented and measured** (§22). `cpu` is still the default renderer.

## 0. First: the decision that reorders everything below

**Plan §23 measured the ProRes 4444 playback stutter and it is the integer tick beat —
cause A — and it is on every file, not just 4444.** Median spacing between doubled frames
is 61–62 against a predicted 62.5, on all six runs, including a 1080p clip whose worst
handler is 3.8ms against a 41.67ms budget. Cost cannot explain a defect identical on a file
with ten times the headroom.

**Only GATE E fixes this.** Items 8 and 9 buy headroom, and §23.4 shows headroom is no
longer the binding constraint on 4444 once the planar path is on (GATE C took its tick
jitter from 11–14ms to 2–3ms and its worst handler from 55.6ms to 37.6ms, inside budget).

So the first thing this session needs is the owner's answer to: **pull GATE E (plan §8 item
11, DXGI presentation timing) ahead of items 8–10?** Do not reorder unilaterally — §23.5
records the argument and explicitly leaves the call to him. If the answer is yes, the rest
of this file is the fallback, not the plan.

Two things to have ready when asking:

- The composition rule is already written and must not be renegotiated (§9): **audio stays
  the rate and position authority; vsync becomes the phase authority.** Vsync picks the
  instant, the audio clock picks the frame for that instant. Unifying those under one owner
  is what removed the hold/skip churn in `cd79d49`; giving vsync the "which frame" question
  brings the two-scheduler bug straight back.
- The flip-model swapchain that makes waitable presentation possible landed at GATE B, and
  `Present(0, 0)` today is sync interval 0 — not vsync-throttled, not phase-aligned (§20.5).
- **Making `d3d11` the default is a separate decision** the plan defers to GATE E, but it
  now has a measured benefit attached (§23.4) and the owner has signed off the rendering.

## 1. Step 8 — texture and upload-resource reuse, IF it is still next

Plan §8 item 8. **Do not justify it on release latency.** §22.4a retracted that: the
"6 → 33ms regression" was a gesture artefact — `-Reversals` never landed a full-res frame
in the BGRA config (`dst RGB32/BGRA 640x360`, `dec 0.00`, `sws 0.00`), so it compared a
landing against no landing. Re-run with `-SnapRelease`, where all three configs land
full-res, planar is the **fastest**: 33.7–46.7ms against 55.4–65.6ms. GATE C improved
release latency by ~20ms.

So this step needs a **new motivating measurement before any code**. Candidates, and be
willing to conclude that none of them justifies it yet:

- Per-frame cost *variance* during playback on 4444, now that the mean is 25ms. §23 shows
  handler>budget is already 0 on the planar path, so this may be closed too.
- Textures are recreated on any geometry change (`ensureTexture`, `ensurePlaneTextures`) —
  a resize or a media switch pays three allocations. Measure a resize, not a steady state.
- `D3D11_MAP_WRITE_DISCARD` on ~56MB of DYNAMIC planes asks the driver for fresh backing
  every map. A staging resource plus `CopyResource`, or `MAP_WRITE_NO_OVERWRITE` with
  fencing, is the usual answer — but measure first: playback handoff is 3.9–4.5ms on planar
  against 2.3ms on BGRA, which is 1.6ms, not 27ms.

**Scope discipline:** resource reuse only. GPU scaling is item 9 and changes what the
picture *is*, not just how it is uploaded. Keep the BGRA path working — it is the control
for every planar measurement.

## 2. Carried, unresolved, do not re-open casually

- **The 4K H.264 stall floor.** §22.8 settled that window size dominates (cache capacity
  76 → 22 as the window grows, stalls 46 → 136) and the HUD now carries `win WxH`. What it
  does *not* explain is 46–51 stalls at the smallest geometry against §17.4's `2 of 394`.
  The box now runs `parsecd`, `sunshine`, Steam and Adobe services, and the display is
  5120x1440 @ 239Hz where §18.3 recorded 2560x1440. **If a clean number is wanted, take it
  deliberately** — those closed, one display, window size written down. Otherwise compare
  stalls only within a session at the same `win WxH`.
- **Why the owner notices the beat on 4444 and not 1080p** (§23.6). The beat is identical
  on both. The only measured difference is the cause-B component on the default `cpu`
  renderer. Needs his eye, not another run.
- **Real mixed-monitor DPI is still untested** (§20.4). One display on this box.
- **BT.2020 has no tonemap**, on either path. Known gap.

## Working notes

- github.com is reachable from the Windows box and you can push directly. Verify with
  `git remote -v` and `git rev-list --count @{u}..HEAD` rather than assuming — the
  `~/Claude/Trace` path is macOS-only and handing it to Anj here fails silently.
- Build locally with the VS2022 / Qt 6.10.2 / vcpkg commands in `CLAUDE.md` before pushing.
  Check the configure lines for `audio output enabled` and `D3D11 renderer enabled`.
- Line endings are pinned by `.gitattributes` (`c7c5bda`). If `git status` ever shows mass
  modifications again, check `git ls-files --eol` before believing them.
- **Run any renderer comparison twice** — the first d3d11 run of a session carries a warm-up
  cost large enough to read as a regression (§21.4).
- **`-SnapRelease` for anything about the landing**; `-Reversals` does not guarantee one.
- **Before comparing two numbers, check the two runs did the same work.** `dst`, `dec` and
  `sws` sit on the same HUD lines as the figure being compared, and twice this session they
  were what showed a "regression" was not one.
- `V:\` is live client production storage and is strictly read-only.
- Update `CLAUDE.md` and the plan at the end of the session.
