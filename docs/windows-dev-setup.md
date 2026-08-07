# Windows dev setup (local build for fast iteration)

Goal: build and run Trace directly on the Windows box so a code change takes
~1 minute instead of a full push → GitHub Actions → download → test loop.

**GitHub Actions stays the source of truth for releases.** This local build is
for iteration and performance measurement only. If a local build breaks, it
never blocks a release — push and let CI build the shipping ZIP as usual.

Everything below is the configuration actually verified working on this machine
(2026-08-06). Where it differs from CI, that is called out explicitly rather
than papered over.

---

## Where the repo lives

**`C:\Users\andre\Documents\Claude_Cowork\Trace_Windows`** — copied from the Mac
on 2026-08-06, full git history intact.

Use this folder. Don't clone a second copy: two working copies on one machine is
exactly the situation that produces cross-machine conflicts.

Two one-time fixes, both already applied:

```powershell
git config core.filemode false
git config user.name "Anj Puglise"
git config user.email "andrewpuglise@gmail.com"
```

The first stops the Mac's `core.filemode = true` from making every file look
modified on Windows. The second sets a commit identity — git had none on this
box, and commits fail without it. Both are repo-local, not global.

Not in git (Mac-only, not needed to build): `assets/icons/_old/`,
`assets/icons/*.zip`, `assets/Interface/*.zip`, `.DS_Store` files. Those design
sources exist **only on the Mac** and are not backed up by this repo — worth
copying somewhere safe independently.

---

## What is installed (verified)

| Component | Version / location |
|---|---|
| Visual Studio 2022 **Community** | `C:\Program Files\Microsoft Visual Studio\2022\Community` |
| — workload | Desktop development with C++ (MSVC 14.44.35207, x64) |
| Windows SDK | 10.0.26100.0 |
| Qt | **6.10.2**, kit **`msvc2022_64`** → `C:\Qt\6.10.2\msvc2022_64` |
| CMake | 3.30.5, bundled with Qt → `C:\Qt\Tools\CMake_64\bin\cmake.exe` |
| vcpkg | `C:\vcpkg` |
| FFmpeg (via vcpkg) | **8.1.2** (`avcodec-62`), `C:\vcpkg\installed\x64-windows` |
| Python | 3.12 (only needed if Qt is ever reinstalled via `aqtinstall`) |

Nothing else needs installing. In particular:

- **Do not `winget install Kitware.CMake`.** Qt ships a new-enough CMake
  (3.30.5 ≥ the 3.24 the project requires). It is simply not on `PATH`, so call
  it by full path or add `C:\Qt\Tools\CMake_64\bin` to `PATH` yourself.
- **Do not install VS Build Tools.** Full VS 2022 Community is already here with
  the C++ workload, which is a superset.

`app/CMakeLists.txt` pins no Qt version (`find_package(Qt6 REQUIRED COMPONENTS
Widgets)`), so Qt 6.10.2 configures and builds cleanly with no source changes.

---

## Building from the command line

This is the flow used for all the performance work below, and the one to prefer.

**Configure** (first time only, or after files are added/removed):

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" -S C:\Users\andre\Documents\Claude_Cowork\Trace_Windows -B C:\Users\andre\Documents\Claude_Cowork\Trace_Windows\build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\6.10.2\msvc2022_64" -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
```

**Build** (day to day, rebuilds only what changed — usually seconds):

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build C:\Users\andre\Documents\Claude_Cowork\Trace_Windows\build --config Release --target Trace --parallel
```

**Worked if:** the last line is `Trace.vcxproj -> ...\Release\Trace.exe`, followed
by the vcpkg toolchain copying `avcodec-62.dll`, `avformat-62.dll`,
`avutil-60.dll`, `swscale-9.dll`, `swresample-6.dll` next to it.

**Run:**

```powershell
$env:PATH = "C:\Qt\6.10.2\msvc2022_64\bin;C:\vcpkg\installed\x64-windows\bin;$env:PATH"; C:\Users\andre\Documents\Claude_Cowork\Trace_Windows\build\app\Release\Trace.exe
```

The `$env:PATH` line is needed once per new terminal window — it is what supplies
the Qt runtime DLLs. FFmpeg DLLs are already deployed next to `Trace.exe` by the
vcpkg toolchain, so they don't strictly need the PATH entry.

**Always build Release** for anything perf-related. Debug decode and scaling
timings are meaningless — often 5–10× slower — and trustworthy measurement is
the whole point of the local build.

---

## Building in Visual Studio (CMake as a local folder)

Trace has no `.sln`. Open it with **File → Open → Folder…** and point at the repo
root; VS detects `CMakeLists.txt` and configures automatically.

VS needs to be told where Qt and vcpkg are. Add to `CMakeSettings.json` (or the
CMake settings UI) for the **x64-Release** configuration:

```json
{
  "configurations": [
    {
      "name": "x64-Release",
      "generator": "Visual Studio 17 2022 Win64",
      "configurationType": "Release",
      "cmakeToolchain": "C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "variables": [
        {
          "name": "Qt6_DIR",
          "value": "C:/Qt/6.10.2/msvc2022_64/lib/cmake/Qt6",
          "type": "PATH"
        }
      ]
    }
  ]
}
```

`Qt6_DIR` must point at the **`lib/cmake/Qt6`** directory, not at `C:/Qt` and not
at the kit root. Setting `CMAKE_PREFIX_PATH` to
`C:/Qt/6.10.2/msvc2022_64` works equally well; use one or the other.

Select the **x64-Release** configuration before measuring anything. Debug is the
default and will give useless timings.

---

## Packaging a local build (windeployqt)

The command-line run above relies on `PATH` for Qt DLLs. To produce a
self-contained folder you can move or hand to someone else, run `windeployqt`:

```powershell
& "C:\Qt\6.10.2\msvc2022_64\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw C:\Users\andre\Documents\Claude_Cowork\Trace_Windows\build\app\Release\Trace.exe
```

This copies `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll` and the
`platforms\qwindows.dll` plugin into the same folder. Combined with the FFmpeg
DLLs the vcpkg toolchain already put there, `build\app\Release\` then runs on a
machine with no Qt installed.

Use the `windeployqt.exe` from **the same Qt kit you built against**. Running the
`mingw_64` one against an MSVC build will copy the wrong DLLs.

---

## Local developer build vs the CI packaged build

These are deliberately different and should not be confused:

| | Local build | CI build (`.github/workflows/windows-release.yml`) |
|---|---|---|
| Purpose | Iteration, profiling, perf A/B | The artifact Anj tests and ships |
| Qt | 6.10.2 `msvc2022_64` | **6.7.2** via `install-qt-action` |
| FFmpeg | vcpkg, 8.1.2 (`avcodec-62`) | vcpkg, cached under `VCPKG_CACHE_VERSION` |
| Packaging | manual `windeployqt` | `windeployqt` + verified DLL manifest, uploaded as a folder |
| Authority | none | **source of truth for releases** |

**The Qt versions differ (6.10.2 local vs 6.7.2 in CI).** Trace uses only stable
Qt Widgets APIs and pins no version, and no divergence has been observed — but a
local build passing is not proof CI will pass. Green CI is still the gate.

FFmpeg was realigned in commit `63801ae` by bumping `VCPKG_CACHE_VERSION` to
`v2`, because the `v1` cache still held FFmpeg 7.x (`avcodec-61`) while local
resolves to 8.x. That pins the *cache*, not the version: vcpkg is cloned
unpinned, so a future cache miss can drift again. Pin a vcpkg baseline commit if
exact reproducibility ever matters.

CI artifacts are uploaded as a **folder, never a .zip** — `upload-artifact`
always zips its input, so uploading a zip produced a zip-inside-a-zip with no
runnable app in it. Tagged releases still produce a real ZIP, because release
assets are not re-zipped.

---

## Verified performance baseline

Benchmark clip: **4096×2304, 24 fps, ProRes 4444, `yuva444p12le`**
(`TheraTears_Vial_VFX_v002.mov`, 261 frames). Release build, measured in-app from
the wall clock over the full clip.

| Measure | Value |
|---|---|
| Sustained playback | **~23.76 fps** (~99% of real time) |
| Decode | ~16 ms/frame |
| swscale conversion | ~17 ms/frame |
| QImage detach | **0 ms** (was ~6.4 ms before the buffer-pool fix) |
| drawImage / paintEvent | ~0.53 / ~0.62 ms |
| Handler total | ~33 ms against a 41.67 ms budget |
| Callback period | **42.00 ms**, fixed |
| Late / dropped frames | 0 |

The frame rate is **timer-bound, not work-bound**. There is ~8 ms of genuine idle
headroom per frame. `Qt::PreciseTimer` gets 1 ms scheduling granularity on
Windows, and 1000/24 = 41.6667 ms is not expressible at 1 ms, so a fixed periodic
interval necessarily lands on 42 ms — a hard ceiling of 1000/42 = **23.8095 fps**.
Measured 23.76 is 99.8% of that ceiling.

Confirmed dead ends, all measured and reverted — **do not retry these**:

- 6 ms polling tick with accumulator gating — 23.01 fps
- adaptive per-frame single-shot timer — 23.57 fps
- `QChronoTimer` at the exact 41,666,666 ns frame duration — 23.76 fps,
  identical: it accepts nanoseconds, but the OS still schedules at 1 ms, so the
  period stayed 42.00 ms

**Exact 24.000 fps is deferred to the future GPU/vsync renderer**, where the
presentation clock comes from the display rather than an OS timer. The remaining
~1% is timer quantization, not CPU cost, and is uniform — it never drops or
reorders a frame.

### Perf A/B switches

Environment variables the decoder reads (see `CLAUDE.md`):

```powershell
$env:TRACE_PERF_FAST_CONVERT = "1"      # force fast sws flags
$env:TRACE_PERF_ACCURATE_CONVERT = "1"  # force accurate sws flags
$env:TRACE_SEEK_CACHE_WINDOW = "4"      # force reverse-cache fill count
$env:TRACE_KEEP_ALPHA = "1"             # disable alpha-plane stripping

Remove-Item Env:\TRACE_PERF_FAST_CONVERT  # clear one
```

Press **Enter** in the app to toggle the diagnostics HUD, which reports the stage
timings, presented fps, drift, jitter and frame-cycle breakdown above.
(`Ctrl+Enter` is fullscreen. `I` toggles a `showInfo` flag that is currently not
wired to anything — it has no visible effect.)

---

## Working across two machines

The repo lives on the Mac and on Windows. **Run `git pull` before starting work
on either machine.** That's the whole rule.

Claude's sandbox cannot push (the proxy blocks github.com), so commits are made
locally and pushed by hand:

```powershell
git push origin main
```

If you forget to pull and end up with commits on both sides, don't untangle it
manually — say which machine has the work you want to keep.

---

## When something breaks

Copy the whole terminal output rather than summarizing it. Running Claude on the
Windows box means it can read the errors directly.

| Symptom | Cause |
|---|---|
| `cmake: not found` | Qt's CMake isn't on `PATH` — call it by full path |
| CMake can't find Qt | `Qt6_DIR` must end in `lib/cmake/Qt6`; `CMAKE_PREFIX_PATH` must be the kit root `C:\Qt\6.10.2\msvc2022_64` |
| Link errors about `__imp_` symbols, or MSVC/GCC mangling mismatches | **mingw_64 Qt linked against an MSVC build** — see below |
| App exits immediately, no window, no error | Qt runtime DLLs missing — run the `$env:PATH` line or `windeployqt` |
| `This application failed to start because no Qt platform plugin could be initialized` | `platforms\qwindows.dll` missing — `windeployqt` supplies it |
| Crash on startup after copying DLLs by hand | Qt DLLs mixed across builds/versions — see below |
| Builds but no video plays | FFmpeg not found at configure time — check `FFMPEG_AVCODEC_LIBRARY` in `build/CMakeCache.txt` |
| Playback much slower than expected | Built Debug instead of Release |
| `LNK1104: cannot open file Trace.exe` | The app is still running — close it before rebuilding |

### mingw_64 Qt with MSVC

`C:\Qt\6.10.2` contains **two** kits: `mingw_64` and `msvc2022_64`. They are not
interchangeable. Pointing `CMAKE_PREFIX_PATH` or `Qt6_DIR` at `mingw_64` while
building with MSVC produces link errors, or — worse — configures and then fails
at runtime, because the two compilers use incompatible C++ ABIs and name
mangling.

Always use the **`msvc2022_64`** path. If a `Qt6_DIR` has `mingw` anywhere in it,
that is the bug. The same applies to `windeployqt.exe`: use the one under
`msvc2022_64\bin`.

Note the kit is `msvc2022_64`, not `msvc2019_64` — Qt 6.10 dropped the older
toolchain naming. Anything referencing `msvc2019_64` is stale.

### Missing Qt runtime DLLs

`Trace.exe` links Qt6Core/Gui/Widgets dynamically. Without them on `PATH` or
beside the executable, Windows fails the load before any Trace code runs — so
there is no error dialog from the app, and often none at all when launched from
Explorer. Launch from a terminal to see the loader error.

The platform plugin is a separate failure: Qt needs `platforms\qwindows.dll` in a
subdirectory next to the executable, not just the core DLLs.

### Mixing Qt DLLs across builds or versions

Do not hand-copy Qt DLLs from another folder, another Qt version, or the mingw
kit. Qt DLLs are only guaranteed compatible with the exact build they shipped
with; mixing them causes crashes at startup or on first paint, usually with no
useful message.

If a local build starts behaving strangely after copying files around, delete
`build\app\Release\Qt6*.dll` and the `platforms\` folder and re-run
`windeployqt`. To start completely fresh, delete the whole `build` folder and
re-run configure — that is always safe, nothing in `build` is source.
