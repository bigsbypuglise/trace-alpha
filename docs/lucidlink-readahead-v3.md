# LucidLink read-ahead, third attempt — built, correctness-verified, NOT validated against real remote storage

2026-08-15, unattended session (no live LucidLink access, no attended machine time). Code in
`src/core/MediaIoSource.{h,cpp}`. Default OFF (`TRACE_IO_READAHEAD=1` to enable). Harness at
`scripts/measure/readahead_ab.ps1`.

## What this is, stated at its width

A fill-ahead buffer replacing the async worker's one-request-at-a-time behaviour, plus two new
diagnostic knobs (`TRACE_IO_INJECT_KBPS`, `TRACE_IO_INJECT_DELAY_MS`) for reproducing a slow-link
read pattern on a **local** file. Everything measured in this document is a **synthetic
comparison on local media with an injected delay** — it reproduces the read *pattern* of a slow
remote link, not LucidLink itself. CLAUDE.md records that a prior session calibrated an injected
delay against one real cold LucidLink read (11.63 vs 11.35 fps, 3% off); this session had no way
to repeat that calibration — no live remote access, and the instruction for this session was
explicit that a real remote validation gap should be stopped on and reported, not papered over
with a substitute presented as the real thing. **Treat every number below as relative (read-ahead
on vs off, under the identical synthetic link) and not as a LucidLink performance prediction.**

**What real remote validation needs, to close this out**: either live access to a LucidLink mount
with a nominated test file (`V:\` is live client production storage and strictly read-only per
CLAUDE.md — Anj has to nominate the file, the same rule the Share-menu work followed), or a
delay/bandwidth figure Anj is willing to hand over from a real cold-read measurement so the
synthetic knobs can be calibrated without touching the mount.

## Design

Three failure modes from the two prior (reverted, uncommitted) attempts, read from CLAUDE.md's
postmortem, and how this attempt avoids each:

- **v1 opened a second reader and corrupted its own buffer.** This design has exactly one
  fill-ahead buffer, owned by the same worker thread that already owns the `QFile` handle
  (`MediaIoSource::Impl::workerLoop`), never a second decoder or a second file handle.
- **v2 served `min(requested, available)` and fragmented every read**, dropping sequentiality
  91.1% and driving demuxer seeks 2 → 25. This design **blocks until the full requested range is
  buffered** (`readPacket`'s wait loop), matching the "next experiment" the postmortem proposed —
  never a short read, except when a single FFmpeg request genuinely exceeds the configured
  capacity (see the hang below).
- **Neither prior attempt is otherwise comparable**: this one reuses `MediaIoSource`'s existing
  cancellation contract (`generation`, bumped by `cancelOutstanding()`) unchanged — a read-ahead
  serve still runs to completion and is reported stale, never abandoned mid-buffer, exactly as the
  existing per-request async path already does.

Mechanism: the worker fills sequentially ahead of `impl->pos` in `raChunkBytes`-sized reads (env
`TRACE_IO_READAHEAD_CHUNK_KB`, default 4MB) up to `raCapacityBytes` (env
`TRACE_IO_READAHEAD_MB`, default 24MB — see below for why). `readPacket` serves from the buffer:
immediately if already present (`bufferHits`), or waits (with the existing `stallPump` so the UI
event loop stays alive) if not. A request outside the buffered range — a real seek, including the
very first read of a file — discards the buffer and refills from the new position
(`raRebases`). Compaction of the consumed prefix is deferred and amortised, not per-read.

## Two bugs found by testing, not by inspection

**The default fill chunk size (256KB) was catastrophic under a fixed-latency model.** The first
test used `TRACE_IO_INJECT_KBPS` (a bandwidth cap) and looked plausible; the moment
`TRACE_IO_INJECT_DELAY_MS` (a fixed per-call latency — the model CLAUDE.md's prior session
actually used, "an injected per-read delay") was tried, read-ahead performance **collapsed**: 9
reads in 10 seconds at 1053ms average latency, worse than doing nothing. Filling a 24MB buffer in
256KB chunks under an 80ms-per-call cost is 96 calls × 80ms = 7.7 seconds just to fill it once.
Under a per-call latency, the fill chunk must be large enough to amortise the round trip; default
raised to 4MB, exposed as `TRACE_IO_READAHEAD_CHUNK_KB` since the right value depends on the
link's actual latency/bandwidth tradeoff, which this session cannot measure for real.

**A single FFmpeg read bigger than the buffer's capacity could hang.** FFmpeg bypasses its own
AVIO buffer for large reads — CLAUDE.md records single reads up to ~11.5MB on a 9K ProRes plate.
If a request wants more than the buffer will ever hold, waiting for "the full request" is waiting
for something that can never happen once the worker's fill loop pauses at capacity. Fixed by
bounding what a single read waits for at the capacity ceiling (`target = min(want,
raCapacityBytes)`, serving fragmented only in that specific case) — reproduced and confirmed fixed
with `TRACE_IO_READAHEAD_MB=1` against ~2.4MB ProRes 4444 packets (hung for 8+ seconds before the
fix, closed cleanly in 300ms after).

**A second, subtler version of the same class of bug survived the first fix**: with capacity
sized so a fragmented read consumes the buffer's entire contents in one shot, the amortised
compaction threshold (`consumedAfter > raChunkBytes`) lands exactly on its own boundary when
`raChunkBytes == raCapacityBytes` and never fires — the worker's fill loop stays paused waiting
for room that only compaction could create, forever. Reproduced with the same 1MB-cap/2.4MB-packet
case (it still hung after the first fix). Fixed by also compacting immediately whenever the buffer
is sitting at capacity, regardless of the amortisation threshold, and waking the worker rather than
leaving it to its 50ms poll.

Both are the class of bug this project's postmortems keep finding: **correct in the common case,
wrong at a boundary the common case never reaches** — a small-buffer-vs-large-packet combination
that a "does it play a normal file" smoke test would never exercise. Found by deliberately testing
capacity below a real packet size, not by code review.

## Correctness, verified

Pixel-identical output (`abdiff.ps1`, 0% differing, max channel delta 0) between
`TRACE_IO_READAHEAD=0` and `=1` on the same local file (`TRACE_REMOTE_IO=1` forcing both through
the async worker) at:

- Frame 60 reached by 60 forward steps, no injected delay.
- A mixed forward/backward/forward sequence (80 → −35 → +20 steps) exercising multiple buffer
  rebases, under `TRACE_IO_INJECT_DELAY_MS=40`.

**One harness lesson worth keeping**: the first attempt at the mixed-direction test used a rapid
15ms-interval key-press cadence and reported 99.998% different pixels — alarming, and wrong. Rapid
presses coalesce (documented, pre-existing behaviour: "each press supersedes the landing in
flight"), and under different I/O latency the two configurations resolved a *different number* of
the rapid presses before landing, so they stopped on different frames — a timing artifact of the
test, not a correctness bug in the mechanism. Slowing the cadence to 300ms (safely above the
injected 40ms plus decode) made both configurations land on the identical frame, and the diff came
back 0%. **A rapid-fire key-press test is not a byte-correctness test unless the cadence is proven
slow enough for the result to be deterministic regardless of the thing being A/B'd.**

Default-off regression: local playback (no env overrides at all, 1080p H.264) after these changes
reads `100.0%`-equivalent — `presented 23.81/24.00 (99.2%)`, `io play ... seq 100.0% ... stall 0`,
same as any unmodified run. The local synchronous path is untouched by this work; only the code
reached when a worker exists (remote storage, or `TRACE_REMOTE_IO=1`) changed at all.

## Results (synthetic — see caveat above)

4K ProRes 422 HQ (`2_4K_ProRes_422HQ/Barritas_16x9_Shot_040-080_v005_ALT_1.mov`), 10s play,
`TRACE_RT_DROP=0` (see below):

| injected per-read latency | off: reads / MB / avg latency | read-ahead: reads / MB / avg latency | hit rate |
|---|---|---|---|
| 20ms | 212 / 497.1MB / 22.4ms | 212 / 497.1MB / **0.79ms** | 209/212 (98.6%) |
| 80ms | 106 / 230.9MB / 87.9ms | 194 / 458.8MB / **30.2ms** | 108/194 (55.7%) |

At 20ms the file finishes inside the window either way (not throughput-bound), and read-ahead's
win is entirely in **caller-visible latency per read**, not total data moved. At 80ms the link is
slow enough relative to this file that read-ahead **roughly doubles delivered data** in the same
wall-clock window by hiding most read latency behind decode/paint work instead of paying it
serially. Neither number should be read as "LucidLink is Nms away" — they establish only that the
mechanism does what it is built to do, under a link whose only defect is latency.

**`TRACE_RT_DROP=0` is necessary for this specific A/B and is not part of read-ahead itself.** The
first attempt, without it, showed the *opposite* result on ProRes 4444 (a heavier file where
decode is already near the frame budget) — 400 Mbps bandwidth cap, `off` read 116 times with 114
"seeks". The real-time drop mechanism (2026-08-13) asks for a **jump** whenever decode+I/O
together miss the frame deadline, and on intra-only media a jump is genuine random access, which a
read-ahead rebase correctly treats as "discard the buffer" — every dropped-and-jumped frame nukes
the prefetch. That is real interaction between two features, not a bug in either; it is disabled
here so this experiment measures I/O read-ahead in isolation rather than a compound of two
mechanisms. **A session validating read-ahead against real playback (not this isolated I/O
benchmark) needs to re-include RT-drop and treat any interaction as a finding in its own right.**

## Knobs added, all default off / inert

- `TRACE_IO_READAHEAD=1` — enable the fill-ahead buffer in place of the per-request path. Inert
  unless a worker exists (remote storage, or `TRACE_REMOTE_IO=1`).
- `TRACE_IO_READAHEAD_MB=N` — buffer capacity, default 24 (comfortably above the largest single
  read observed in this project, ~11.5MB on a 9K plate; a request that exceeds it is still served
  correctly, fragmented, per the hang fix above).
- `TRACE_IO_READAHEAD_CHUNK_KB=N` — fill granularity, default 4096 (4MB). The number that matters
  most under a latency-dominated link; too small and the fixed per-call cost is paid many times
  over just to fill the buffer once (measured above).
- `TRACE_IO_INJECT_KBPS=N` — synthetic bandwidth cap in kbit/s. Read-ahead cannot beat this
  ("buffering cannot beat bandwidth" is already established project doctrine); it models a link
  that is fundamentally too slow for the file, which is a different case from the one below.
- `TRACE_IO_INJECT_DELAY_MS=N` — synthetic fixed per-read-call latency, independent of bytes. This
  is the model that matches what read-ahead is actually for (hiding round-trip cost behind
  overlapped work) and the model CLAUDE.md's prior session used.
- `TRACE_IO_LOG=1` — appends one line per file close to `%TEMP%\trace_iolog.txt` with
  `bufferHits`/`raRebases` plus the existing read/seek/stall/latency counters, for Playback and
  Seek phases. Added because reading these off an HUD screenshot needs OCR and would still miss
  whichever field scrolled off; a log file gives a script exact values to diff. Skips closes with
  zero reads (several spare/probe decoder instances close per launch with nothing read; without
  the skip, a script reading the last line can land on one of those instead of the real session).

## What is NOT done

- **Not validated against real LucidLink.** The absolute latency/bandwidth figures above are
  synthetic. Do not quote them as LucidLink performance in any future session.
- **Not wired into the shipping default.** `TRACE_IO_READAHEAD` stays off. Turning it on is an
  owner decision that should follow a real validation pass, per the pattern the rest of this
  project's experimental knobs already follow.
- **No decision on default `TRACE_IO_READAHEAD_CHUNK_KB`/`_MB` for a real link** — those were
  chosen to be safe defaults (large enough not to reproduce the 256KB collapse, small enough not
  to be a large memory cost), not tuned against a real cold-remote profile.
