# Scrub and playback reliability — the open phase

Owner decision, 2026-08-19. **This outranks the UI roadmap and everything else.** A review
player that scrubs unpredictably is not a review player.

The goal is stated as a property of a *population*, not of a file: **exceptional, frame-accurate
scrubbing and smooth playback for any H.264 MP4 and any ProRes, across resolutions, frame
rates, GOP structures, durations and machines.** It is only finishable once the population's
behaviour is known, which is why part 1 exists.

---

## Read this before planning anything

**Four scrub faults have been reported. Three are fixed, and each had a completely different
cause:**

| report | cause | fix |
|---|---|---|
| 720p ComfyUI MP4 | a cross-thread round trip per frame; frames cost 0.12ms against a 3.56ms delivery interval — 97% round trip | batch up to 4 consecutive frames per request (`be9f7ec`) |
| 4K Seedance 9:16 HEVC | exactly one keyframe in 97 frames; every miss walked from the head **on the UI thread** | the exact landing moved to the worker (`cc8e638`) |
| `M&M_TopGun_1080` on a Threadripper | `Present` blocking at the display; the chain painted once per decoded frame and was capped at the refresh | the scrub paint gate (`b830027`), then a ×1.05 margin (`ebc1fa7`) |
| **`Universe_rc07_I_9x16_Online` and others, on the DEV BOX** | **open** | — |

**That table is the actual problem.** Three unrelated mechanisms, each invisible until a
particular file met a particular machine. Fixing the fourth the same way will not converge, and
it is why this phase starts with characterisation rather than a fix.

### What is already built — do NOT reimplement any of it

A brief written without this project's history would ask for these. They exist, they are
validated, and rebuilding them would be a regression:

- **Stale-request supersession.** `requestGeneration_` is monotonic, bumped by
  `supersedeInFlightRequests()` on every target change; `loadCurrentFrame` captures the
  generation and discards results whose generation moved. The scrub worker holds a **lease** on
  the single decoder, reclaimed through one choke point, `reclaimDecoder()`. `stale-blocked`
  reads 0 by construction. **An older frame cannot replace a newer request.**
- **Keyframe-aware seeking with pre-roll.** After any seek the first decoded frame's index is
  resolved from its PTS (`seekResolvePending`) and decode continues forward to the true target.
  Every seek is frame-exact — `delta 0` appears in every report in this repo, on every machine,
  including the Threadripper at its worst.
- **Bounded caching.** The reverse cache is **byte-budgeted at 384MB**, verified bounded (1357
  inserts / 1245 evictions, never over), discarded on file change, and playback-neutral. 768MB
  was measured and is past the knee.
- **Decode and file I/O off the UI thread for random access.** The scrub worker and the async
  landing. **Playback deliberately decodes synchronously on the UI thread** — that is a
  validated decision after **two reverted async attempts** that broke frame ordering
  (`a171e3a`/`1d280eb`, reverted `9cd2a0c`/`a2f7999`). Do not "fix" it.
- **Directional prefetch.** Measured and **declined twice**, with reasons — the worker has no
  idle time on the files that hitch. Re-derive before proposing it; do not assume it is missing.
- **Full instrumentation.** Requested-versus-displayed (`target`/`shown`/`delta`), seek and
  decode latency, dropped/repeated frames (`drop`/`rep`/`skip`), cache hit rate and occupancy
  (`rev-hit`, `cache N/M`), keyframe distance (`ra-walk`, `walk max`), UI-thread blocking
  (`ui gap`), paint gap and paint cost, worker round trip split into wake/deliver/overhead.
  **All on the HUD and in `--scrub-selftest`.** Do not add temporary instrumentation before
  checking whether the field exists.

### Two facts about the environment that changed everything this week

- **The dev box has a 239.999Hz panel, and it has been flattering every scrub measurement in
  this project.** Most users are on 60Hz. `scripts/measure/setrefresh.ps1` changes the mode.
- **`Present` blocking is a driver/composition property, not a refresh rate.** The dev box at a
  true 60Hz does *not* block; the Threadripper does. `TRACE_PRESENT_SYNC=1` is the in-binary
  model of that class and reproduces the fault here.

**A media-pipeline investigation that stops at "rendering" would have missed the last confirmed
fault entirely.** The presentation path is in scope.

---

## Part 1 — characterise the population. No code changes.

Run `--scrub-selftest` over **every H.264 MP4 and every ProRes file in
`Trace_Testing_Assets`**, on a build at or after `ebc1fa7`, run plain (HUD hidden — the HUD
itself costs ~2.5ms of delivery latency per request).

**Three passes over the whole set**, because the display is now known to be a variable:

1. the panel at **240Hz** (the historical baseline)
2. the panel at **60Hz** via `setrefresh.ps1` (what most users have)
3. **`TRACE_PRESENT_SYNC=1`** (the blocking-present class)

**One table.** Per file: codec, resolution, fps, duration, frame count, **GOP structure from
`ffprobe`** (keyframe count and the gap distribution), audio present, `win`/`display`. Per leg
per pass: `supply`, `p2p end/max`, `behind end/max`, `hitch`, `paint gap`, `paint cost`,
`dec f/s`, `decode/frame`, `round trip` and its `overhead`, `batch achieved`, `rev-hit`,
`seeks`, `ra-walk`, `walk max`, `delta`.

Add ProRes to the sweep even though it is believed healthy — *believed* is the word that needs
removing.

**Then read the table and report. Do not propose a fix in this session.** The question is not
"what is wrong with this file" but **"which files and conditions are bad, and what do they
share that the good ones do not."** If two of nine are bad, the shared property is the bug. If
most are bad at 60Hz and fine at 240, the scrub path has a systemic weakness the dev box has
been hiding.

**The standing hypothesis to test against the table**, offered so it can be refuted rather than
assumed: the scrub path carries **five fixed constants — batch cap 4, walk budget 8ms, coalesce
12ms, ease 0.50, seek-walk fill 60ms — all tuned on this box, all expressed as time or count
budgets whose meaning changes with the machine and the media.** The two mechanisms in the same
path that *do* adapt to measured cost — §15's sampling stride and the paint gate — have
produced no reports. That contrast is the hypothesis.

---

## Part 2 — the class-level fix, in a later session

Only after the table is read and the owner has seen it.

- **Fix the class, never the file.** No file-specific branches, no special cases keyed on a
  filename, resolution or codec profile.
- **The safety property is not negotiable:** any adaptive replacement for a tuned constant
  **must reproduce the tuned value on this box under the conditions it was tuned in.** If an
  adaptive batch converges to anything but 4 at 240Hz here, it is wrong.
- **Preserve, without exception:** frame-exact release and stepping (`delta 0`), frame ordering,
  the never-skip-a-frame rule and its four sanctioned exceptions, source timebase and the exact
  rational rate, rotation and colour metadata, audio sync, and the bounded memory footprint.
- **Do not rewrite the media subsystem** unless the table shows the current design cannot meet
  the goal. If it does, say so with the evidence, and constrain the rewrite to the decoder and
  scrub path — the renderer, transport, window and interface are out of scope.
- **Proxy or optimised media is an OWNER DECISION, not an implementation choice.** It is a
  feature with generation, storage and invalidation, and it cuts against the product's stated
  pillars. None of the three fixed faults would have been helped by it. If the table shows a
  file class that genuinely cannot be scrubbed natively, present it as a proposal with a cost.

---

## Part 3 — make the claim verifiable, not felt

The reason this keeps recurring is that "MP4s scrub well" has never been checkable. Turn the
part 1 sweep into a **standing regression with an explicit pass bar per file**, re-runnable
after every change. That is what closes this permanently and what turns the owner's goal from a
hope into a table.

## Acceptance

- The frame displayed after a scrub release matches the requested position **exactly** — this
  is stricter than "within one frame" and is the existing, measured guarantee. `delta 0`.
- A stale result never replaces a newer request.
- ProRes scrubbing is immediate across the supported set.
- H.264 scrubbing tracks the pointer on **all three display passes**, not only at 240Hz.
- Playback shows no visible drops, stalls or audio drift; cadence stays in its recorded class.
- Repeated scrubbing causes no unbounded memory growth, deadlock or UI freeze.
- The standing regression — cadence, `-SnapRelease`, lifecycle, the 25 transitions,
  `emptystate`, `uiatree` — is flat, with a control binary built and hash-verified beside it.

## Two working rules this project has paid for

**Build the control.** Four times a plausible reading has been corrected by a control binary or
a negative control rather than by reading the code harder. A measurement without one is an
opinion.

**A recorded figure is a record, not a baseline, unless a control was taken beside it.** The
machine, the display and the window size all move. Quote `win`, `display` and the refresh rate
with every number.
