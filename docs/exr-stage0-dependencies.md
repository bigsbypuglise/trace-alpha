# EXR + colour management, stage 0: dependencies

Record of the dependency stage only (2026-08-23). The assessment this implements
is `docs/exr-ocio-plan.md`; read that first. **Nothing in the Color Transform
feature was built, the View menu was not touched, and the vcpkg pin did not
move.**

## What landed

- `openimageio[opencolorio]:x64-windows` and its 19-package graph, from the
  pinned vcpkg tree `17f35ad2418007a895ced8a4cece4ab34068a58d`.
- `app/CMakeLists.txt`: `find_package(OpenImageIO CONFIG QUIET)` +
  `find_package(OpenColorIO CONFIG QUIET)`, linking `OpenImageIO::OpenImageIO`
  and `OpenColorIO::OpenColorIO`, defining `TRACE_WITH_OIIO` / `TRACE_WITH_OCIO`,
  and printing a configure-time status line per library.
- The CI workflow installs both, packages their runtime DLLs, and asserts at
  configure time that they were found.
- One pre-existing build defect fixed in passing -- see "The FFmpeg trap" below.

**Versions, read off the running libraries rather than the port manifests:**
OpenColorIO **2.5.2**, OpenImageIO **3.1.14.0**.

2.5.2 is the version that fixes **CVE-2026-42450** (stack buffer overflows in the
`.cube` / `.spi1d` / `.spi3d` / `.lut` parsers). That is the code path this whole
phase exposes to arbitrary user files, so **the pin must never move backwards.**
It did not move here.

## Cost

| | wall | what happened |
|---|---|---|
| cold (empty binary cache) | **5.1 min** | 20 packages built from source; OIIO itself 1.1 min, OCIO 56 s |
| warm (binary cache present) | **7 s** | 19 packages restored from `%LOCALAPPDATA%\vcpkg\archives` in 988 ms |

The binary cache grew **7 -> 27 entries, 64.3 -> 164.9 MB**. Compile time for
Trace itself is unchanged; the package grows **95.4 -> 118.3 MB**.

**CI pays this differently, and that is why `--clean-after-build` is now passed.**
CI caches the whole vcpkg *tree* through `actions/cache`, so on a cache hit the
install step is skipped entirely and the added cost is zero build time. On a cache
miss it pays the ~5 min. What the flag protects is the cache *entry size*: on the
dev box the OIIO graph leaves `buildtrees` at **3,795 MB** and `packages` at
**610 MB** against an `installed/` of **610 MB**, so an uncleaned entry is ~6 GB
of which ~4.4 GB is intermediate rubble. GitHub allows 10 GB per repository and
evicts LRU, so a 6 GB entry would start pushing the Qt and ffmin caches out --
which is exactly the "a green run and a red run differ only by whether the cache
aged out" scar the `VCPKG_PIN` comment already records.

**And the flag is now demonstrated rather than reasoned.** The first CI run after
this change was a cold, from-source install of both ports, and its cache upload
was **268,812,508 B (256 MB compressed)** under
`vcpkg-v4-windows-2022-deps-x64-17f35ad2...`. Against an uncleaned tree of ~6 GB
that is the flag doing exactly what it is here for, on the machine that matters.
(The `~453 MB` and `~719 MB` cache figures in the same log are the **Qt** and
**minimal-FFmpeg** caches being *restored*, not this one -- easy to misattribute,
so they are named here.)

## THE FFmpeg TRAP: `build/` had been linking the wrong FFmpeg, and the status line could not say so

Found while establishing the control, and **not caused by this change** -- the
pre-change binary has it too.

`build/CMakeCache.txt` read:

```
TRACE_FFMPEG_ROOT      = C:/tw_ff9/out                       <- the flag WAS set
FFMPEG_AVCODEC_LIBRARY = C:/vcpkg/.../debug/lib/avcodec.lib  <- but this is vcpkg
```

`find_path` / `find_library` write **cache** entries and return immediately when
one is already set. A build tree first configured without `TRACE_FFMPEG_ROOT`
keeps whatever it found on the default path forever, and adding the flag to a
later configure silently changes nothing. So the local `build/` tree was linking
**vcpkg's MSVC FFmpeg 8.1.2** -- and its **debug** import library -- while
CLAUDE.md records dev and CI as both running the minimal GCC 9.0.1.

Measured on the exe import tables:

| binary | imports |
|---|---|
| pre-change `build/` (02:06) | `avcodec-62` `avformat-62` `avutil-60` `swscale-9` `swresample-6` |
| fresh `build-control/` | `avcodec-63` `avformat-63` `avutil-61` `swscale-10` `swresample-7` |

62 is vcpkg 8.1.2; 63 is ffmin 9.0.1. That is the ~18%-slower-decode toolchain,
under a status line saying otherwise.

**Two fixes, both in `app/CMakeLists.txt`:** the cache entries are `unset` before
searching when `TRACE_FFMPEG_ROOT` is set, so the flag is authoritative on every
configure; and the status line now prints the **resolved** library path instead of
echoing the request back. The old line printed correctly throughout the fault --
it was a claim that could not fail. Same rule as the HUD's `renderer` / `planar` /
`font` / `strip` fields.

**Consequence for the record:** any local measurement taken in that build tree
since the FFmpeg 9.0.1 upgrade was taken on 8.1.2 MSVC. Every figure in *this*
document was taken after the fix, with both binaries confirmed on `avcodec-63`.

## `/utf-8` -- it was already there

The plan flagged that `fmt` adds `/utf-8` as a PUBLIC MSVC option, so it lands on
Trace's own translation units. It does. Two findings:

1. **It is required, not cosmetic.** `fmt/base.h` carries
   `static_assert(..., "Unicode support requires compiling with /utf-8")`, and
   OIIO's `imageio.h` reaches it. A hand-rolled `cl` line without `/utf-8` fails
   to compile against OIIO headers.
2. **It changes nothing, because Qt already sets it.** Comparing the two generated
   vcxproj files: the control (no OIIO) already carries `-Zc:__cplusplus -utf-8`
   from Qt's mkspec; the new build carries `-Zc:__cplusplus -utf-8 /utf-8`.
   `-utf-8` and `/utf-8` are the same MSVC flag. fmt's contribution is a literal
   duplicate of one the build already had.

(Separately, every `.cpp`/`.h`/`.hlsl`/`.rc` under `src/` and `app/` is pure
ASCII, so the flag would have been a no-op regardless -- but the duplicate-flag
finding is the stronger one, because it needs no assumption about future sources.)

## The two questions

### (a) Does a single-layer EXR open? YES -- no code change needed.

`StillImageLoader::loadExr` already existed behind `TRACE_WITH_OIIO`; supplying
the library is the whole of it. Tested on an **isolated copy** of
`R2_OP_Stacks_01_00000.exr` so `SequenceParser` could find no siblings:

```
Still | solo.exr | 1920x1080 ch:4 | Frame: 0/0
```

Picture correct on screen.

### (b) Does the SEQUENCE path accept EXR? YES -- and it never needed wiring.

**There is no extension whitelist anywhere on that path.** `MainWindow::openPath`
branches `mp4`/`mov` to video and the audio extensions to audio; *everything else*
falls through to `SequenceParser::detect()`, whose regex `^(.*?)(\d+)(\.[^.]+)$`
matches any suffix. `ImageSequenceFrameSource` then calls `StillImageLoader::load`
per frame, which dispatches `.exr` to `loadExr`. So EXR was wired the moment OIIO
was present.

The 217-frame folder opens as a sequence and **plays end to end**:

```
Sequence | R2_OP_Stacks_01 | 1920x1080 | Frame: 216/216 | Seconds: 9.000
```

217 frames at 24 fps is 9.04 s, so it kept up -- warm from disk, ~1.6 GB in ~9 s
(~180 MB/s), which is the plan's section 4 estimate met on this NVMe. **This is
not a cadence claim:** the sequence path exposes no `drop`/`rephase` counters, and
the run was warm. Cold, and on slower storage, is unmeasured.

### Confirmed from the file headers, via OIIO

`R2_OP_Stacks_01_00000.exr` -- `1920x1080`, `nchannels=3`, channels `R G B`,
format `half`, compression `piz`, **`chromaticities` ABSENT**.
`icecream0000.exr` (Beauty_Only) -- same but compression `dwaa`.

All four of the plan's file findings hold.

### One discrepancy, reported not fixed

The HUD reads `ch:4` for a 3-channel file. `MainWindow.cpp:6539` hard-codes
`info.channels = 4` on the frame-handoff path -- honest about the **display
buffer** (always RGBA after handoff) and misleading as a label. It only becomes
*visibly* wrong now that EXR opens. What that field should say is a stage-2
decision (the plan asks for raw channel names in the HUD), so it is left alone.

## OCIO is linked but not yet referenced

Trace's import table carries `OpenImageIO.dll` and `OpenImageIO_Util.dll` but
**not** `OpenColorIO.dll` -- no Trace symbol touches OCIO yet, so the linker emits
no direct import. `OpenColorIO_2_5.dll` still ships, transitively, because OIIO
imports it. **`TRACE_WITH_OCIO=1` is therefore currently a claim nothing in the
product tests.**

Proven outside the product instead, with a standalone probe compiled against the
same headers and import libraries:

```
OCIO runtime version : 2.5.2
OIIO runtime version : 3.1.14.0
config name          : Redshift_3_0
config major version : 2
scene_linear role    : ACEScg
getColorSpaceFromFilepath(x.exr) : Raw
cube 0.18,0.18,0.18 -> 0.14753 0.15410 0.15887
```

Two of those lines are the plan's finding 1, now **measured rather than
predicted**: the `scene_linear` role gives `ACEScg` (the right default) and
`getColorSpaceFromFilepath` gives `Raw` (the wrong one). The last line is the real
ARRI `.cube` parsing and applying through `FileTransform` -- i.e. the
CVE-2026-42450 parser path, working.

**Recommended stage-1 entry item:** an `--ocio-selftest` alongside the renderer
and shape selftests, so CI asserts OCIO initialises rather than merely linking.
Not built here -- it is a new code path and this stage was scoped to dependencies.

## Regression -- flat

Physical panel **5120x1440 @ 239.999 Hz** (confirmed via `refresh.ps1`; not
Parsec). Control = **identical source**, configured with
`-DCMAKE_DISABLE_FIND_PACKAGE_OpenImageIO=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenColorIO=ON`,
i.e. the two libraries removed from the link line and nothing else. Both binaries
confirmed distinct by hash and confirmed to import the same FFmpeg (`avcodec-63`),
so the libraries are the only variable.

- **`scrubbar.ps1` full pool: PASS -- 22 files, 88 legs, `delta 0` throughout**,
  5.0 min warm. Jeep (the recorded flake-risk boundary file) passed, as did WeLo
  and Universe (the decisive ones).
- **4K H.264 cadence x2, new vs control**, `TRACE_NO_AUDIO=1`, scratch INI,
  `win 1278x1083` / `display 1278x719 filtered x2`:

  | | NEW r1 | NEW r2 | CTL r1 | CTL r2 |
  |---|---|---|---|---|
  | presented | 24.00 (100.0%) | 23.99 (100.0%) | 23.99 (100.0%) | 24.00 (100.0%) |
  | handler>budget | 0 of 119 (max 4.8) | 0 of 119 (max 4.5) | 0 of 119 (max 4.8) | 0 of 119 (max 4.5) |
  | buckets | ~1x 119 | ~1x 119 | ~1x 119 | ~1x 119 |

  `drop 0`, `rephase 0`, `tick-late 0`, `tick-stall 0`, `sizemove 0` on all four.
  The `handler>budget` max matches to the digit, rep for rep.
- **ProRes 4444 cadence x2 each: 99.8% on all four**, 261 frames, `drop 0`,
  `rephase 0`, `handler>budget 0 of 260`, handler avg 20.74-20.75 throughout. The
  `<0.9x` bucket reads 3/4 on the new binary against 1/1 on the control --
  **inside this file's recorded 1-10 run-to-run span**, and CLAUDE.md's standing
  instruction is not to chase a 4444 bucket spread. Presented rate, `drop` and
  `handler>budget` are identical.
- **4K 60fps cadence x2 each -- the tightest budget in the pool at 16.67 ms, so the
  place any added per-frame cost would show first: 100.0% on all four reps**, 162
  frames, `drop 0`, `rephase 0`, `tick-late 0`, `tick-stall 0`,
  `handler>budget 0 of 161` with max **4.2/4.4 (new)** against **4.3/4.4
  (control)**. The `<0.9x` bucket spans 0-7 on the new binary and 1-5 on the
  control, i.e. overlapping run-to-run spread on both.
- **4444 `scrub -SnapRelease`, both binaries:** `target 261 shown 261 delta 0`,
  full-res `YUV444P12 planar`, `walk 0f`, `hitch 0`, `land 0`; release 22.4 ms
  (new) against 22.6 ms (control).
- **4K H.264 reversal drag (real mouse), both binaries:** `scrub exact target 2
  shown 2 delta 0`, **`hitch 1`** on both -- the recorded figure -- `seeks 4`,
  `behind 0/24f`, `supply 95/96%`, release 49.9 ms (new) against 51.0 ms (control).
- **`transitions.ps1 -All`: 25 of 25 PASS** on the new binary
  (`TRACE_TRANSPORT_BAR=1`, `TRACE_HUD=0`, on the clip its header names).
- **Selftests on both binaries:** `renderer=d3d11 fellback=0 planar=1`,
  `OK - 11 shapes x 4 scale factors` (44 rows), `--scrub-selftest` exit 0 on all
  88 legs. `verify_trace_assets.py --strict --no-pillow` green at **33 embedded
  files** -- unchanged, as it must be, since no asset moved.
- **Launch to window, 7 reps each, alternating**, no media (the purest DLL-load
  measurement): new **min 909.8 / med 936.5 / max 1034.2 ms** against control
  **min 907.8 / med 945.4 / max 966.8 ms**. The new binary's median sits *below*
  the control's; the spread within each set is ~60 ms. The 19 extra DLLs cost
  nothing measurable at startup.

The three `warning C4834` lines (`VideoDecoderFFmpeg.cpp:233,257`,
`MediaIoSource.cpp:75`) appear **identically on the control**, so they are
pre-existing and unrelated.

## CI

- Installs `ffmpeg` and `openimageio[opencolorio]`, both with
  `--clean-after-build`; cache key renamed `ffmpeg` -> `deps` and
  `VCPKG_CACHE_VERSION` bumped `v3` -> `v4`, because a v3 entry is a different set
  of packages under the same name.
- **Configure-time assertion** that `OpenImageIO_DIR` and `OpenColorIO_DIR`
  resolved, beside the existing FFmpeg and audio assertions and for their reason:
  `find_package(... QUIET)` is deliberately optional so a toolchain without them
  still builds a working player, which is exactly what makes an unnoticed miss
  possible on the toolchain where they are supposed to be present. **Proven able
  to fail:** the regex passes on `build/CMakeCache.txt` and fails on
  `build-control/CMakeCache.txt`.
- **Packages the vcpkg runtime closure** by copying `*.dll` from the build tree
  next to the exe -- that directory is vcpkg's applocal deployment, i.e. vcpkg's
  own computed transitive closure, rather than a second hand-maintained list here
  that would go stale. The ffmin FFmpeg copy stays **last** so it wins any name
  collision (there is none today: vcpkg is `avcodec-62`, ffmin is `avcodec-63`).
- **Adds the five EXR/colour DLLs to the launchability assertion.**

Validated locally by building a `dist` with exactly the CI sequence and launching
it with `PATH` reduced to `System32`:

- **11 of 11 required files present, 118.3 MB**, and `--renderer-selftest=d3d11`
  exits 0 with `renderer=d3d11 fellback=0 planar=1`.
- **Negative control:** renaming `OpenImageIO.dll` away makes the same launch exit
  **`0xC0000135` (STATUS_DLL_NOT_FOUND)** -- so the DLL copy is load-bearing, and
  without it CI would have shipped an exe that cannot start.

### CI is green, with every verification step read individually

Run [32647950705](https://github.com/bigsbypuglise/trace-alpha/actions/runs/32647950705)
on `6be43f88`, branch `exr-stage0-dependencies`. Step 6 (the cold vcpkg install of
both ports from source) took **37.8 min**; the minimal-FFmpeg cache **hit**, so
that build was skipped; Trace itself compiled in **1.9 min**.

```
derived:   33 embedded files, plus their SVG masters
minimal FFmpeg DLLs: 20.9 MB
dependency check: all DLLs import only Windows system libraries
-- Trace: FFmpeg avcodec resolved to D:/a/_temp/ffmin/out/lib/avcodec.lib
-- Trace: OpenImageIO enabled (EXR reader) 3.1.14.0
-- Trace: OpenColorIO enabled 2.5.2
FFmpeg detected by CMake.
Audio dependencies detected by CMake.
OpenImageIO + OpenColorIO detected by CMake.
Package verified: 11 required files present, 118.3 MB total.
trace-selftest: renderer=d3d11 fellback=0 planar=1
trace-shape: OK - 11 shapes x 4 scale factors
```

Four of those are worth pointing at. **`fellback=0` is the hardware path** -- the
check accepts `d3d11 (warp)` by prefix, so a WARP pass would look identical in the
step's tick and different in that line. **`118.3 MB` matches the locally built
`dist` to the digit**, so the packaging is deterministic across the two machines.
**The new resolved-path FFmpeg line works on CI and names the ffmin tree**, which
is the line that would have caught the local fault. And OCIO **v2.5.2** and OIIO
**v3.1.14.0** were fetched and built from source on the runner, so the versions
are confirmed there and not only on the dev box.

## Observed in passing, for stage 2 -- not fixed, not blockers

The 27-channel `icecream_passes0000.exr` **opens and draws its root composite
correctly** (`Sequence | 1920x1080 | Frame: 0/96`, 97 frames detected). Three
things about how it gets there are worth having written down before stage 2
starts, because none of them is visible on screen today.

1. **Three channel-naming conventions exist in this one asset set**, which is
   stronger than the plan's finding 2. Root layer `R G B`; named layers
   `Beauty.red` / `Beauty.green` / `Beauty.blue`; and the Cryptomatte file
   `CryptoMaterial.R` / `.G` / `.B` / `.A` -- uppercase, *with* alpha. A grouper
   written against any one of the three finds nothing in the other two.
2. **`loadExr` reads every channel of the file.** `read_image(0, 0, 0, channels,
   TypeDesc::FLOAT, ...)` with `channels = spec.nchannels` allocates
   `width x height x nchannels` floats -- on this file that is
   1920 x 1080 x 27 x 4 = **~224 MB per frame**, of which 24 MB is used. Correct
   today, and the wrong shape for a 97-frame sequence.
3. **Channel index 3 is taken as alpha regardless of what it is.** On this file
   channel 3 is `Beauty.red`, so the display buffer's alpha is the beauty pass's
   red channel. It is invisible right now because the draw path ignores alpha,
   which is exactly what makes it a trap: it will surface the moment alpha starts
   mattering rather than at the point the mistake is made.

Alpha is also currently pushed through the same `pow(x, 1/2.2)` as the colour
channels in `loadExr`'s `toDisplay8`, which is wrong in principle (alpha is not
display-referred) and equally invisible for the same reason.
