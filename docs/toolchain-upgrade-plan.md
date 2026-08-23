> ## SUPERSEDED — read this banner before the document
>
> **This is the ASSESSMENT written 2026-08-22, retained as the record of what was
> proposed. The work is DONE. Where this document and the records disagree, the
> records win:** `docs/ffmpeg-9-upgrade.md` and `docs/qt-6.11-upgrade.md`.
>
> It was written at 17:31 on a tree that did not include the five commits on
> `diag/audio-pull-gap-instrument`, the last of which landed at 16:57 the same
> afternoon. That is the source of its largest error.
>
> ### Disproven
>
> 1. **"The next cache miss fails the release on a filename."** It would not. The
>    shipped artifact does not take its FFmpeg from vcpkg at all —
>    `TRACE_FFMPEG_ROOT` points CMake at the minimal MinGW tree with
>    `NO_DEFAULT_PATH`, and `FFMPEG_BIN` is overwritten to that tree before
>    packaging. A vcpkg drift to 9.0.1 would have cost build time and bootstrap
>    risk, not a failed publication.
> 2. **"The workflow hardcodes the 8.x names in two places — line 194 and line
>    326."** Only line 194 (the *minimal* build's own assertion, pinned by its
>    build script). Line 326 already matched by prefix, `"$lib*.dll"`, and could
>    not have failed. **The pin was still right — for reproducibility, not for
>    defusing a bomb.**
> 3. **Step 3's entire premise.** "CI to 6.10.2 first… the only change that
>    isolates the hypothesis" — that experiment had already been run and refuted.
>    Qt 6.7.2 was measured on a CI artifact (run 32599463708, backend verified as
>    `QWindowsAudioSink`, no MMCSS thread) and gaps **identically** to 6.10.2.
>    This session added 6.11.2 to that list. **The Qt version is not the
>    variable**, across three majors. 6.10.2 was skipped as a milestone by owner
>    decision.
> 4. **QTBUG-132285 as the attribution for the move-drag picture cost.** Refuted.
>    It is repaint-on-**move**, and the fault's worst reproduction is a
>    **motionless** caption press — 3.7x worse than dragging. Measured on 6.11.2:
>    the stall is unchanged. Do not re-propose this attribution.
> 5. **Three site counts were grep hits, not code sites.** `WM_DPICHANGED` is
>    **one** handler, not eight. `WS_EX_LAYERED` is **two** calls, and
>    `TopChrome.cpp:227` already applies it with `SetWindowLongPtrW` after
>    creation — which is exactly what QTBUG-135333 changed Qt to do, so that
>    named risk could never bite.
> 6. **The frameless `showMaximized()`/`showNormal()` rework does not apply.**
>    Trace is not frameless; roadmap step 12 was closed as declined and the native
>    title bar stays.
>
> ### Held up
>
> - vcpkg cloned unpinned behind a 7-day cache is a real reproducibility hole,
>   and pinning it first was the right order.
> - FFmpeg 9.0.1 needs **no source change**. Verified independently: all 147
>   FFmpeg identifiers under `src/` resolve against the 9.0.1 headers, and the one
>   real removal is already behind `LIBAVCODEC_VERSION_MAJOR >= 61`.
> - The 9.0.1 sonames listed here are correct.
> - TLS-verify-by-default is a non-event for Trace.
> - The aqtinstall blocker is real, and the named fix is the right one — though CI
>   pins a **commit** rather than the branch this document suggests.
> - The Qt version gap really was a correctness problem. That reasoning stands on
>   the sink rewrite alone, independently of the drag-bug hypothesis it was
>   attached to.
>
> ### One thing it did not predict
>
> Losing the vcpkg/ffmin soname match **closed a real trap**: while the sonames
> matched, vcpkg's applocal deployment silently copied its own MSVC DLLs beside a
> local build configured against `TRACE_FFMPEG_ROOT`, so that build ran vcpkg's
> FFmpeg rather than the GCC one it linked. It caught out this session's first
> control build. At 9.0.1 the sonames no longer collide and the substitution is
> impossible.

---

# Toolchain upgrade: Qt 6.11.2 and FFmpeg 9.0.1 — assessment and sequencing

Owner request, 2026-08-22: move Trace to the current Qt and the current FFmpeg.
Both versions are real and current — **Qt 6.11.2** (2026-08-18) and **FFmpeg
9.0.1 "Lei"** (2026-08-12).

This document is the assessment, not the port. Nothing is edited yet. It exists
because the request turned up one thing that is more urgent than the request.

## The urgent finding: CI is already armed to break

`.github/workflows/windows-release.yml` clones vcpkg **unpinned**
(`git clone --depth 1 https://github.com/microsoft/vcpkg.git`, line 123) and the
cache that hides that fact **expires after 7 days idle** (line 108). vcpkg's
`ports/ffmpeg` on master is **already at 9.0.1**, whose sonames are
`avcodec-63` / `avformat-63` / `avutil-61` / `swscale-10` / `swresample-7`.

The workflow hardcodes the 8.x names in two places — the dependency gate at
line 194 and the package verification at line 326. **The next cache miss builds
9.0.1 and the release fails on a filename.**

This is not hypothetical and it is not new: the comment at line 34 records the
same thing happening once already — "the v1 cache still held FFmpeg 7.x
(avcodec-61) while local moved to 8.x". The cache was bumped; the unpinned clone
that caused it was not.

**So the vcpkg pin comes first, before any upgrade work.** Not because it is
part of the upgrade, but because until it lands, no CI result means anything —
a green run and a red run can differ only by which day the cache expired.

## FFmpeg 9.0.1: cheap, and cheaper than staying

The full `FF_API_*` guard diff from 8.1 to 9.0 removes nine things from avcodec,
two from avformat, five from avutil, three from avfilter, one from avdevice —
and **nothing from swscale or swresample**. Checked against Trace's actual call
surface, **none of it is used**:

| removed in 9.0 | in Trace |
|---|---|
| `AVCodecContext.properties`, `FF_CODEC_PROPERTY_*` | no |
| `av_opt_set_int_list`, `av_int_list_length` | no |
| `av_opt_ptr`, `av_mod_uintp2` | no |
| `av_parser_init` signature change to `enum AVCodecID` | no parser use at all |
| `av_stream_get_codec_timebase`, `AVTimebaseSource` | no |

The one place that could have hurt is already guarded. `VideoDecoderFFmpeg.cpp:1320`
picks `av_packet_side_data_get` over `av_stream_get_side_data` behind
`LIBAVCODEC_VERSION_MAJOR >= 61`, with a comment that says exactly why: "CI
resolves its own vcpkg baseline, so the one place they could differ should not
be a compile error discovered on a runner." That instinct was right and it pays
out here — on 9.0 the guard takes the modern branch and the legacy branch is
never compiled.

**TLS verification defaulting on is a non-event for Trace.** It is the loudest
item in 9.0's coverage, but it only bites code that opens network URLs through
libavformat. Trace reads media through its own instrumented `AVIOContext`
(`VideoDecoderFFmpeg.cpp:879` — "Media is read through our own AVIOContext
rather than FFmpeg's file" path) and never hands FFmpeg a URL.

`sws_alloc_context` / `sws_init_context` / `sws_setColorspaceDetails` all still
exist in 9.0. `sws_init_context` is doc-deprecated in favour of
`sws_frame_setup` and the new `SwsContext.scaler` field does nothing on the
legacy stateful path — so the existing scaler code keeps working and keeps
producing identical output. Worth a follow-up, not part of this port.

**So the FFmpeg work is strings, not code:**

- `windows-release.yml` line 194 and line 326 — the six DLL names
- `scripts/build-ffmpeg/build-minimal-ffmpeg.ps1` line 62 — `n8.1.x` to `n9.0.1`,
  and the soname comment beneath it
- `scripts/build-ffmpeg/THIRD-PARTY-NOTICES.md` — the DLL table

**Then re-measure, because the minimal build exists for a number.** The
MinGW/LGPL FFmpeg was built to buy ~18% of decode throughput on large ProRes
over vcpkg's, at identical output. That delta is a property of two specific
builds of 8.1 and **does not automatically survive to 9.0**. Re-run
`scripts/measure/decbench` on both before concluding the replacement is still
worth its complexity.

## Qt: the version gap is a correctness bug, not a lag

The dev box builds **6.10.2**. CI pins **6.7.2**. That gap has been treated as
untidy. It is worse than untidy.

The Windows audio sink was rewritten in `bca6400a8` ("Wasapi: rework QAudioSink"),
which shipped in **6.9.1** and **6.8.4-lts** — a *patch* release, not only the
6.10 feature release. 6.7.2 is on the far side of it. The two implementations
do not agree on the meaning of the values `AudioOutput::advanceClock()` uses as
the master clock:

| | 6.7.2 | 6.9.1+ |
|---|---|---|
| `processedUSecs()` | everything fed in — runs ~500ms ahead of audible | frames actually handed to WASAPI — ~one device period ahead |
| `bytesFree()` | free space in the **WASAPI client buffer** | free space in the **ring buffer** |
| `setBufferSize()` | passed straight to `IAudioClient::Initialize` | sizes the ring buffer; the client is sized at the device period |
| `stop()` | immediate, same as `reset()` | asynchronous drain; `reset()` is the immediate one |

`advanceClock()` reads `processedUSecs()` and computes in-flight as
`bufferSize() - bytesFree()` (`AudioOutput.cpp:734-735`). Both terms change
meaning across that boundary. **Every release ZIP ever shipped has run a
differently-behaving clock than the build the clock was tuned on.**

And it is the standing explanation for the open drag bug: on 6.7.2 the sink
pulls from a `QTimer` on the creating thread — the main thread, since
`AudioOutput::open` creates it at `AudioOutput.cpp:544`. Inside a modal move
loop `WM_TIMER` is only synthesised when the input queue is empty, so a real
hand drag starves it. On 6.9.1+ a dedicated MMCSS thread drains a ring buffer
and the same drag cannot starve the device. See `docs/audio-window-drag.md` —
eleven configurations that did not reproduce, all of them on 6.10.2.

**Nuance worth carrying, because it is the part that is easy to get wrong:** the
rewrite did *not* move the `QIODevice` read off the app thread.
`pullFromQIODeviceImpl()` still opens with `Q_ASSERT(thread()->isCurrentThread())`.
What changed is the trigger — an auto-reset event from the audio thread instead
of a timer — and the cushion, now a 250ms ring buffer. A blocked GUI thread can
still starve it, it just has far more room. So moving the sink to its own
`QThread` is not made pointless by the upgrade; it is made *cheaper to evaluate*,
because it stops being the only candidate fix.

### Sequencing: CI to 6.10.2 first, as its own step

Do not go straight to 6.11.2. Move CI to **exactly what dev already runs**, ship
nothing else in that change, and run the drag harness against the resulting ZIP.

That step is worth more than it costs. It is the only change that isolates the
hypothesis — if the drag bug dies at 6.10.2 with nothing else touched, the
mechanism is proved, and it is proved on the artifact rather than on a
developer's box.

What that step newly exposes on the shipped artifact, by crossing 6.8 and 6.9:

- **6.8: the Windows font backend default moved GDI to DirectWrite.** Text
  metrics shift. Trace has hand-tuned chrome and typography from the interface
  pass, so expect visual drift and check it deliberately. Escape hatch if it is
  bad: `QT_QPA_PLATFORM=windows:fontengine=gdi`.
- **6.9: legacy Windows mouse handling was removed** — everything arrives via
  `WM_POINTER`, and `-nowmpointer` no longer exists. Trace has seven
  `nativeEvent` sites and does its own hit regions on the transport strip. This
  is a retest, not a port, but it is the one most likely to surprise.
- 6.9: frameless windows respect `WORKAREA` when maximized; 6.10: a
  destroyed-and-recreated `QWindow` now keeps its position and size; 6.10:
  `WS_EX_LAYERED` decided at creation and centralised. Trace has four
  `WS_EX_LAYERED` sites and twelve `winId()` sites, so these are in its path.

The dev box has already crossed all of that and survived. That is evidence, not
proof — the artifact is a different binary.

### Then 6.11.2, on both at once

**Blocker to clear first.** `jurplel/install-qt-action@v4` (latest v4.3.1) pins
`aqtversion: ==3.3.*`, and **no released aqtinstall can fetch 6.11.x** — Qt
changed its download-repo folder layout at 6.11 and the fix is on aqtinstall's
master, not in any tag. The workflow needs:

```yaml
- uses: jurplel/install-qt-action@v4
  with:
    version: '6.11.2'
    arch: 'win64_msvc2022_64'
    aqtsource: 'git+https://github.com/miurahr/aqtinstall.git'
```

Toolchain minimums are already met: MSVC >= 1930 (VS 2022), CMake >= 3.22
(the project asks 3.24), C++17 (the project sets C++20).

What 6.11 actually moves, that Trace is in the path of:

- **`WS_EX_LAYERED` is no longer set in `CreateWindowEx`** and is added
  afterwards via `SetWindowLongPtr` (QTBUG-135333). Four sites here.
- **Frameless `showMaximized()`/`showNormal()` reworked in 6.11.2 itself**
  (QTBUG-145092): `MoveWindow()` replaced by `ShowWindow()` plus a maximize rect
  constrained in `WM_GETMINMAXINFO`. Retest maximize, restore, Win+Up and the
  system menu.
- **`WM_DPICHANGED` now passes the suggested rect into `checkForScreenChanged()`.**
  Eight `WM_DPICHANGED` sites here, and mixed-monitor DPI is the standing beta
  gate that has never executed. Qt moving this code is a reason to run that gate
  as part of the upgrade rather than after it.
- **QTBUG-132285 / QTBUG-115992 — "window containing native windows window is
  excessively repainted on move", fixed in 6.11.0.** That is a precise
  description of the measured cost in `docs/audio-window-drag.md`: a move drag
  costs 66 dropped frames, 107.4ms of tick jitter and 98.1% presented, against
  zero for a resize drag, and it reproduces on `TRACE_RENDERER=cpu` so it is not
  the swapchain. That asymmetry was left unattributed. **This may be its
  attribution, and the upgrade may close it for free.** Re-run the same harness
  legs after 6.11.2 and compare against the recorded figures.

Trace has **no `requestUpdate()` call**, so the 6.9 change that drives D3D11
window updates from a vblank watcher thread does not apply.

## Order, and why this order

1. **Pin vcpkg.** Until this lands, every CI result is a coin flip on cache age.
2. **FFmpeg 9.0.1.** Independent of Qt, no code changes, and it converts the
   armed time bomb into a deliberate move. Re-run `decbench` on both FFmpeg
   builds afterwards.
3. **CI Qt 6.7.2 to 6.10.2, alone.** The experiment. Run `audiodrag.ps1` against
   the resulting ZIP with an `-Mode idle` control. Check fonts and mouse input.
4. **Qt 6.11.2 on dev and CI together.** Clear the aqtinstall blocker first.
5. **Re-validate on the artifact** — drag harness, decode benchmarks, and the
   mixed-monitor DPI gate.

Steps 1 and 2 are safe to do in one sitting. Step 3 is the one that pays. Steps
4 and 5 are where the retest time actually goes.

## What this does not decide

Whether to move `QAudioSink` to its own thread. If step 3 kills the drag bug,
that question stops being urgent — but the ring buffer can still be starved by
a blocked GUI thread, so it does not stop being a question. Defer it until there
is a measurement on 6.11.2 that argues for it.
