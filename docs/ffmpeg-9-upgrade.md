# FFmpeg 9.0.1: the shipping minimal build, measured

Step 2 of the toolchain modernisation (`docs/toolchain-upgrade-plan.md`).
The shipping dependency — the minimal MinGW/GCC LGPL FFmpeg built by
`scripts/build-ffmpeg/build-minimal-ffmpeg.ps1` — moves **n8.1.2 → n9.0.1**.

**vcpkg is NOT the shipping FFmpeg and is deliberately left at 8.1.2.**
`TRACE_FFMPEG_ROOT` points CMake at the minimal tree with `NO_DEFAULT_PATH`, and
the workflow overwrites `FFMPEG_BIN` with that tree's `bin` before packaging. The
vcpkg install remains the toolchain host and the documented revert path, pinned
at `17f35ad2…` by the preceding commit.

## Sonames, read from the source rather than a release note

| | 8.1.2 | 9.0.1 |
|---|---|---|
| avcodec | 62 | **63** (63.1.101) |
| avformat | 62 | **63** |
| avutil | 60 | **61** |
| swresample | 6 | **7** |
| swscale | 9 | **10** |

Taken out of `libav*/version*.h` in the extracted tree. The workflow asserts all
five after the build, so a wrong guess is a red CI step rather than a runtime
surprise.

## No code changed, and that was verified rather than assumed

Every FFmpeg-looking identifier under `src/` was extracted (147 of them) and
checked against the 9.0.1 headers. Four did not resolve, and all four are
explained:

- `AVAILABLE`, `av_color_` — regex artefacts, both inside comments. The real
  calls (`av_color_primaries_name`, `av_color_transfer_name`) resolve.
- `sws_flags` — an AVOption *name* passed to `av_opt_set_int`, not a header
  symbol. Confirmed still present in `libswscale/options.c` at 9.0.1.
- `av_stream_get_side_data` — the one genuine 9.0 removal Trace mentions, and it
  is already behind `LIBAVCODEC_VERSION_MAJOR >= 61` in `VideoDecoderFFmpeg.cpp`.
  At 63 the guard takes the `av_packet_side_data_get` branch and the removed
  function is never compiled. The guard's own comment predicted exactly this.

TLS-verify-by-default is a non-event: media is read through Trace's own
`AVIOContext` and FFmpeg is never handed a URL.

## Decode throughput: the minimal build's justification survives

The minimal build exists to buy decode throughput over vcpkg's MSVC build. That
was a property of two 8.1 builds; it does not automatically survive a version
change, so it was re-measured — **decomposed into toolchain and version rather
than measured as one confounded step**, since vcpkg stays at 8.1.2 while the
shipping build moves to 9.0.1.

`decbench <file> <fps> 32 4` (sustained phase, slice t=32, 4 consecutive passes),
physical panel box, nothing else running. Worst-pass fps:

| arm | avcodec | 8K 4444 XQ | 4K 4444 |
|---|---|---|---|
| vcpkg 8.1.2 (MSVC) | 62.28.102 | 19.68 | 78.37 |
| ffmin 8.1.2 (GCC) — ships today | 62.28.102 | 23.48 | 92.45 |
| **ffmin 9.0.1 (GCC) — this change** | **63.1.101** | **23.75** | **92.33** |

- **Toolchain, version held constant**: +19.3% (8K), **+18.0%** (4K) — the
  recorded ~18% reproduces.
- **Version, toolchain held constant**: +1.1% (8K), −0.1% (4K) — nothing, which
  is what the build script's own header already says about 8.1 vs master.
- **Shipping decision** (ffmin 9.0.1 vs vcpkg 8.1.2): +20.7% (8K), +17.8% (4K).

**The minimal build is still worth its complexity.** The gain is the toolchain,
it is unchanged by the version bump, and it is if anything marginally larger.

## Decode is bit-identical

Frames hashed straight out of the decoder, single-threaded for determinism, and
hashed **at `width × componentsize` per row rather than at `linesize`** — stride
padding is allocator-dependent and a stride-unaware diff is how a previous
session produced 399 phantom differing pixels on four unrelated files.

24 frames per file, 8.1.2 vs 9.0.1:

| file | format | result |
|---|---|---|
| 4K ProRes 4444 | yuva444p12le | identical |
| 4K ProRes 422 HQ | yuv422p10le | identical |
| 8K ProRes 4444 XQ | yuva444p12le | identical |
| 4K H.264 | yuv420p | identical |
| 1080p H.264 | yuv420p | identical |
| 4K HEVC (Seedance) | yuv420p10le | identical |
| 720p libx264 (ComfyUI) | yuv420p | identical |

**Proven able to fail before the result was believed**: consecutive frames of one
file hash to 4 distinct values, and frame 0 of two different files differs. An
instrument that can only report "identical" reports nothing.

## Regression, physical panel 5120x1440 @ 239.999Hz

Control built from the same commit against ffmin 8.1.2, run beside every leg.

- **Full-pool scrub bar `scrubbar.ps1`: PASS — 22 files, 88 legs, `delta 0`
  throughout**, exit 0. The exactness contract across the whole population.
- **4K H.264 cadence ×2**: 100.0/100.0%, `drop 0`, `rephase 0`,
  `handler>budget 0 of 119`, all 119 gaps `~1x` — identical to the control.
- **4K ProRes 4444 cadence ×6 per build**: 99.8% every rep on both, `drop 0`,
  `handler>budget 0 of 260` every rep. The cadence buckets look smeared on any
  single 4444 rep (`<0.9x` ranged 1–10) — **that is this file's own run-to-run
  variance and the control does it identically**, which is why it was run six
  times rather than argued about.
- **Audio-mastered cadence ×2** (1080p with sound, the swresample-7 path):
  **99.6/99.6% on both builds**, `drop 0`, `rephase 0`, `0 of 240`, identical
  buckets. Audio is the one subsystem a soname bump could have broken quietly.
- Renderer selftest `renderer=d3d11 fellback=0 planar=1`, shape selftest
  `OK - 11 shapes x 4 scale factors`, both builds.
- **Packaging**: dist assembled as CI assembles it, all six required files
  present, and the packaged `Trace.exe` launches and passes both selftests **with
  `PATH` reduced to `System32`** — the technique that would have caught the
  `libwinpthread` import locally instead of on a runner. Dependency check clean:
  all five DLLs import only Windows system libraries. DLL set 20.8 → **20.9 MB**,
  well under the workflow's 40 MB leak gate.

## Two things that changed shape, recorded so they are not rediscovered

**The vcpkg-soname match is gone, deliberately.** n8.1.x was chosen so the
minimal build's sonames matched vcpkg's, which made swapping it in a DLL swap
rather than a Trace rebuild. With vcpkg at 8.1.2 and the shipping build at 9.0.1
they no longer share sonames or headers, so that A/B now needs a reconfigure. The
revert path is unaffected — dropping `-DTRACE_FFMPEG_ROOT` still returns the
build to vcpkg.

**And losing it closed a real local trap.** vcpkg's applocal deployment copies
DLLs matching the exe's imports out of the vcpkg tree. While the sonames matched,
a *local* build configured against `TRACE_FFMPEG_ROOT` got **vcpkg's MSVC DLLs
copied beside it** and silently ran those instead of the GCC ones it linked
against — which is how the first control build in this session was set up, and it
would have made the toolchain A/B compare vcpkg with vcpkg. CI was never exposed
(it copies `FFMPEG_BIN` over the top explicitly). At 9.0.1 the sonames no longer
collide, so the substitution cannot happen: the right DLLs are present or the app
does not start.
