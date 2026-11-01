# F1 — D3D11VA hardware decode: built, measured, and a net loss on this machine

**2026-10-31. Branch `mp4-hwdec`. `TRACE_HWDEC` ships OFF and the recommendation
is that it stays off.** This closes F1 from `docs/mp4-playback-review.md` with a
measurement rather than with a design.

The short version: hardware decode works, its pixels are bit-identical to
software decode, and turning it on costs ~19ms per 4K frame. On the one clip in
the pool with no headroom it goes from **100.0% of real time to 63.8%, dropping
57 frames**. The premise F1 was scheduled on is half wrong, and the half that is
wrong is the decisive one.

---

## 0. Display state — read this before quoting anything here

**Parsec was running and the panel was not in its recorded mode.** The active
display path was the **Parsec Virtual Display Adapter at 1920x1200 @ 60.000Hz**
(`QueryDisplayConfig`), and the 4090's own output read **1024x768 @ 60Hz** —
not the recorded 5120x1440 @ 239.999Hz. `DwmGetCompositionTimingInfo` is
unavailable on this box as always.

So:

* **No figure here is a cadence, smoothness or pacing baseline.** The
  `presented`/`drop` figures below are quoted only as the *within-pair*
  difference between two runs taken minutes apart at an identical window size,
  which is what an A/B needs; neither column is comparable to any recorded
  baseline.
* **Per-frame cost terms and pixel comparisons are valid**, because each A/B
  pair ran at the same window geometry (`win 936x891`, `display 936x527` on
  every leg) on one binary.
* The one thing this display configuration could have invalidated is which
  adapter the D3D11 device lands on, and that was checked rather than assumed —
  see §2.

---

## 1. What is built

`TRACE_HWDEC=d3d11va`. Unset, `0` or `off` is software, which is the default.

* **The spelling is a device NAME, parsed by `av_hwdevice_find_type_by_name`.**
  Not a boolean, because D3D11VA is one of several hardware paths FFmpeg knows
  and the name is how a second one would ever be selected. A typo therefore
  cannot silently mean "on".
* **Whether a codec can take it is FFmpeg's question**, asked through
  `avcodec_get_hw_config`, not a list of codec names in this repo to maintain.
  A build without D3D11VA, or a codec with no hardware path in this build,
  declines there.
* **The per-file fallback is mostly free, and that is a property of the
  `get_format` callback rather than of any code that handles failure.** The
  decoder offers a format list; `chooseHwFormat` returns the hardware format
  when it is on offer and **the first software format otherwise**. A profile or
  pixel format the GPU cannot take never appears in that list, so the file
  simply decodes in software from its first frame. Returning the hardware
  format regardless is the obvious implementation, and it is what turns that
  case into a decode failure.
* **The one failure that does not self-resolve is a copy-back that fails**, and
  that path reopens the codec without hardware, re-seeks and re-walks, once per
  media. It is flagged rather than acted on in place, because the failure is
  noticed deep inside the decode walk where freeing the codec would pull the
  context out from under the loop reading it.
* **`openCodecContext(allowHw, error)` is extracted** so open and fallback are
  one code path. Two copies would be two threading policies, two
  colour-parameter transfers, and a fallback that quietly stops matching the
  real open.
* **The copy-back is at the single `avcodec_receive_frame` site.** There is
  exactly one in the file, which is what makes "the frame is CPU memory before
  anything downstream sees it" a statement rather than an invariant to maintain
  at a dozen call sites.

### What is reported, and why it takes two fields

`decodePath` is a statement about **setup**; `hwFramesTransferred` is the
statement about **frames**. A build that created the device, opened the codec
and then never received a hardware frame would print `d3d11va` on the HUD and in
the issue mail with nothing behind it — which is this project's own recorded
failure mode (`stalls 0 of 0` printed on frames carrying `period max 512ms`).
Neither half is the instrument; the pair is.

* HUD line 2: **`hwxfer last/avg`**, beside `dec` and deliberately not inside
  it. Folding a PCIe readback into decode time reports the readback as a decode
  win, which is the shape of the mistake `ffmpeg -f null` already made once
  here.
* HUD line 3: **`dec-path`**, beside `thr` — both are constant for the life of a
  media open.
* `--scrub-selftest` `media:` line: **`dec-path <path> hwf <n> | gpu <adapter>`**.
  The selftest resolves no window, so it is the one instrument here that cannot
  be voided by the tooltip trap.

**Shipping cost with the knob unset: one cached `qgetenv` at open and one int
compare per received frame.** The transfer frame is allocated only on the
hardware path, so the software build does not pay even a frame header.

---

## 2. It works, on the right GPU, across the classes that have a hardware path

`--scrub-selftest`, knob on, `media:` line read off each run:

| file | codec / format | `dec-path` |
|---|---|---|
| `1_4K_ProRes_4444` | `prores yuva444p12le` 4096x2304 | **software (d3d11va: this build's prores decoder has no d3d11va path)** · hwf 0 |
| `4_4K_H264_MP4` | `h264 yuv420p` 3840x2160 24 | `d3d11va` · hwf 122 |
| `7_4k_H264_60FPS_MP4` | `h264 yuv420p` 3840x2160 60 | `d3d11va` · hwf 176 |
| `16_4kSeedance` | `hevc yuv420p10le` 2160x3840 | `d3d11va` · hwf 194 |
| `14_720P_Comfyui_mp4` | `h264 yuv420p` 1280x720 | `d3d11va` · hwf 361 |
| `24_iPhone_BurnIn\IMG_0516` | `hevc yuv420p` 3840x2160 | `d3d11va` · hwf 287 / 351 |
| `24_iPhone_BurnIn\IMG_0563` | `hevc yuv420p` | `d3d11va` · hwf 301 |

**`gpu NVIDIA GeForce RTX 4090` on every row, not the Parsec virtual adapter.**
That was the specific risk of measuring this while Parsec held the display: a
D3D11 device created on a virtual display adapter has no NVDEC behind it, and
"hardware decode is slow" and "hardware decode is on the wrong GPU" look
identical from everything else on the line.

**The ProRes row is the decline path proving it can fire**, on real pool media
rather than synthetically, and it names the reason rather than printing a bare
`software`.

**10-bit works** (`yuv420p10le` → P010), which was worth establishing
separately: 10 bits is where a wrong range or shift shows, and the review
flagged 4:2:2 10-bit as unsupported by NVDEC — 4:2:0 10-bit is not, and the
hardware config list told the truth about which is which with no code here
asking.

### What the shipped FFmpeg carries

Confirmed against the shipped `build/app/Release/avcodec-63.dll`, not the gyan
build on PATH: `d3d11va` present (8 occurrences), `hevc_d3d11va` present,
`dxva2` present. **`nvdec` absent** — this build has the DirectX hardware
accelerators and no CUVID/NVDEC path, which is the right shape anyway since
Trace already owns a D3D11 device.

---

## 3. The colour gate passes, and it passes as an exact zero

Three captures of **frame 0** of the iPhone 4K HEVC clip, same window, one
binary. Video band only, tolerance 2:

| comparison | differing | max channel delta |
|---|---|---|
| sw decode + swscale **vs hw decode + swscale** | **0 of 43127** | **0** |
| sw decode + planar shader vs hw decode + swscale | 10704 (24.82%) | 94 |
| sw decode + planar shader vs sw decode + swscale | 10704 (**24.82%**) | **94** |

**The decoder's pixels are bit-identical.** The third row is what proves it: it
reproduces the second row's figure *to the pixel* with no hardware decode
anywhere in it, so the whole difference is the conversion route and none of it
is the decoder.

**Frame identity was confirmed from the burn-in plate, not assumed.** A max
channel delta of 94 is this project's recorded different-frame signature, so
before reading any of it as colour both captures were cropped and read: both
show **`F 0`** and **`00:00:00.000`**. That is what the plate is for.

### The 24.8% is a real finding about the hardware path, just not a colour one

It is `filtered x3` against `filtered x1` on the HUD. GATE C's planar upload
carries **step 9's box-average reduction**; the BGRA path takes a single
bilinear tap. The copy-back lands in NV12, which `planarLayoutFor` does not
recognise, so the hardware path **forfeits GATE C's planar upload and step 9's
downscale filter together** — a cost *and* a picture-quality regression, on a
decision this project signed off on 2026-08-10.

---

## 4. The cost. Two files, one binary, same window.

### 4K HEVC 8-bit — `IMG_0516_burnin_norot.MOV`, 23.976fps, 41.71ms budget

| per frame | software | `TRACE_HWDEC=d3d11va` |
|---|---|---|
| `src -> dst` | `yuv420p -> YUV420P8 planar` | `nv12 -> RGB32/BGRA` |
| reduction | `filtered x3` | `filtered x1` |
| `dec` last/avg | 0.09 / **0.39** | 0.71 / **0.82** |
| `hwxfer` | 0.00 / **0.00** | 9.68 / **7.23** |
| `sws` | 1.19 / **1.21** | 6.02 / **6.18** |
| `upload` | 0.68 / 0.72 | 1.66 / 1.59 |
| **`handler`** | 2.25 / **2.33** | 15.73 / **15.83** |
| `handler>budget` | 0 of 321 (max 4.2) | 0 of 320 (max 25.1) |
| presented | 23.98 (100.0%) | 23.95 (99.9%) |
| `drop` | 0 | 0 |

**6.8x the per-frame cost.** It still holds real time, because 15.83ms fits
41.71ms — so on this file the knob is merely wasteful rather than visible.

### 4K H.264 60fps — `Jeep_Snowboard_Drone_60FPS.mp4`, 16.67ms budget

The tightest budget in the pool, and therefore the strongest case F1 had.

| per frame | software | `TRACE_HWDEC=d3d11va` |
|---|---|---|
| `dec` | 0.02 / **0.37** | 0.64 / **1.27** |
| `hwxfer` | 0.00 / 0.00 | 11.54 / **9.51** |
| `sws` | 0.95 / **1.24** | 12.09 / **9.78** |
| `upload` | 0.56 / 0.76 | 1.51 / 1.64 |
| presented | **59.98 / 60.00 (100.0%)** | **38.28 / 60.00 (63.8%)** |
| `drop` | **0** | **57** |
| `handler>budget` | **0 of 161** (max 4.4) | **58 of 104** (max 52.5) |
| cadence buckets | `<0.9x 6 ~1x 151 1.1-1.5x 4` | `<0.9x 27 ~1x 9 1.1-1.5x 15 1.5-2.5x 48 >2.5x 5` |

The arithmetic: **+9.5ms readback, +8.5ms swscale, +0.9ms upload — about 19ms
added, against 0.9ms of decode SAVED, which is itself negative.** Hardware
decode made decode dearer.

---

## 5. Why. The premise F1 was scheduled on is half wrong.

F1 reads: *"Playback decode, pixel conversion and GPU upload all run inside the
tick handler on the UI thread — this is the ceiling on every heavy MP4."*

Decode is **dispatched** on the UI thread. It is not **performed** there. With
frame threading at `thread_count` 16, `avcodec_send_packet` queues and
`avcodec_receive_frame` returns a frame finished earlier by a worker, so the
tick pays the handoff and not the decode. Measured: **`dec` averages 0.37ms on
4K H.264 at 60fps and 0.39ms on 4K HEVC** — and the whole handler is 2.33ms of
41.71ms, or 2.0ms of 16.67ms.

CLAUDE.md already records the mechanism from the other direction, in the
intra-only threading note: *"with frame threading the next frame decodes
underneath all of that."* What had not been done is reading the consequence
forward to F1. **There is nothing on these files for hardware decode to win.**

And the second half is structural:

> **Every file in the pool that misses its budget is ProRes, and ProRes has no
> D3D11VA path at all.** The 8K 4444 XQ plate (56.9% of real time, `dec`
> 39.08ms) and the 4K 4444 clip are intra-only; NVDEC does not decode ProRes in
> any build. The hardware decoder covers exactly the class that does not need
> it.

---

## 6. Would zero-copy win? On this machine, no — and the ceiling is computable.

A zero-copy D3D11VA path (decoded texture handed straight to the renderer, a
shared `ID3D11Device`, and NV12/P010 sampling in the GATE C shader) would remove
*both* costs above: no `hwxfer`, and `sws`/`upload` back to the planar figures.
What it cannot remove is the term that went the wrong way:

* 4K 60fps: `dec` **0.37 → 1.27ms**. A perfect zero-copy implementation is
  therefore still **~0.9ms per frame worse than software** on this box, because
  `avcodec_receive_frame` on a hardware decoder is a GPU sync point while the
  software path's is a frame-threading handoff.
* On ProRes it is **exactly zero**, by construction.

That is an estimate, and its basis is stated: the `dec` column measured here,
which may carry some pressure from the copy-back sitting behind it. It is the
best evidence available and it points one way.

**Recommendation: do not build the zero-copy stage.** Keep the knob, default
off, as the control that closes F1 — the same position
`TRACE_LONGGOP_SLICE_THREADS` and `TRACE_INTRA_FRAME_THREADS` hold for the
threading questions they refuted.

### The one configuration where the knob earns its keep

A machine with far fewer cores, where frame-threaded software decode of 4K HEVC
*does* land in the handler. The condition is arithmetic: the copy-back costs
~9.5ms at 4K, so software decode has to cost **more than ~9.5ms in the handler**
on that machine before hardware decode wins — and even then it arrives without
GATE C and without step 9's filter. That is why it is a tester-facing knob and
not a default.

---

## 7. Regression, knob off (the shipping configuration)

The real risk was never the hardware code — it is the `openCodecContext`
extraction, which every open now goes through.

* `scrubbar.ps1` full pool: **PASS — 22 files, 88 legs, `delta 0` throughout**,
  exit 0.
* `mp4matrix.ps1`, the 19-file corpus with burn-in frame identity read from the
  pixels: **`FILES 19 | PASS 17 | FAIL 2`** — **identical to the recorded result
  to the digit**, same two files for the same two recorded reasons:
  `h264_1080p_rot90` unreadable on all 17 legs (by design — the bit strip is
  MSB-first left-to-right and the reader refuses a rotated capture rather than
  returning a confidently reversed number) and `h264_1080p_vfr_subframe` 13
  mismatches at worst delta 395 (the F3 demonstration fixture, recorded and not
  fixed). Every other file: `legs=17 mismatch=0 unreadable=0 worst=0`.
* `--renderer-selftest=d3d11` → `renderer=d3d11 fellback=0 planar=1` ·
  `--window-shape-selftest` → `OK - 11 shapes x 4 scale factors` ·
  `--exr-channels-selftest` → `OK - 14 channel layouts` · `--ocio-selftest` →
  `version=2.5.2 ... moved=1`.
* **The stderr announce proven in all three cases**, including the one that
  reports nothing: `trace-hwdec: d3d11va` on a file that takes it,
  `trace-hwdec: software (d3d11va: this build's prores decoder has no d3d11va
  path)` on one that declines, and **no line at all with the knob unset**.

---

## 8. Not measured

Stated so none of it reads as cleared.

1. **Everything display-dependent**, per §0. No cadence, smoothness, drag
   latency or pointer-to-picture figure was taken, and the `presented` columns
   above are A/B deltas rather than baselines.
2. **The hardware path on the CPU renderer** (`TRACE_RENDERER=cpu`). It should
   work — the copy-back lands in CPU memory and the CPU backend takes BGRA — but
   it was not run.
3. **Whether the ~9.5ms readback is reducible.** `av_hwframe_transfer_data` on
   D3D11 does a GPU→staging copy then a CPU map; a pre-allocated staging texture
   might beat it. Not pursued, because §6's arithmetic does not turn on it: even
   a free readback leaves the path worse than software here.
4. **The mid-stream copy-back fallback has never fired on real media.** The code
   path is written and reviewed; no file in the pool provokes it, because the
   unsupported cases are all caught gracefully by `get_format` instead.
5. **Any machine but this one.** §6's "one configuration where the knob earns
   its keep" is reasoning from the measured readback cost, not an observation.

---

## 9. Two harness slips, recorded

* **A codec-class loop reused its previous `$line` when a run produced no
  output**, so an "8K ProRes" row printed the 4K ProRes row's text. Caught
  because the dimensions in it read `4096x2304` rather than 8K, and the row was
  discarded. A variable only assigned on success reports the last success, never
  a failure.
* **`abdiff` over window captures cannot tell a frame offset from a colour
  difference**, and the first reading of §3 looked like a colour fault. The
  burn-in plate is what settled it. On a clip without one, a colour comparison
  needs the frame index established some other way before any of its numbers
  mean anything.
* **A push to a ref cancels that ref's in-flight CI run, and this session walked
  into it one day after recording it.** The branch push started a build; a
  docs-only push eight minutes later killed it at `Configure CMake` and started
  a second from scratch. CLAUDE.md already carries this from the beta.2 release
  (where it cost a cold ~40-minute vcpkg install) with the remedy stated as "tag
  first, push follow-ups after" — **which reads as a release-day instruction and
  is actually a general one**: batch the pushes, or do not push again until the
  run you care about has finished. Writing a trap down is not the same as
  recognising it in a new costume.
