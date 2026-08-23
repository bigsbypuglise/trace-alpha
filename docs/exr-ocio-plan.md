# EXR + colour management: assessment and staging

Owner brief, 2026-08-23. This is the assessment before the work, written after
reading the actual test files rather than assuming what they contain. Nothing is
built yet.

## What the test files actually are

Every row below was read out of the file headers, not inferred from the paths.

| file | size | layout |
|---|---|---|
| `R2_OP_Stacks_01_00000.exr` | 1920x1080, 7.8 MB x **217 frames** | single-part, **3 channels** `R G B` half, **PIZ** |
| `icecream_passes0000.exr` | 1920x1080, 35 MB x **97 frames** | single-part, **27 channels**, **DWAA** |
| `AOV_SETUP_AOV_Cryptomatte0000.exr` | 1920x1080, 4.6 MB x 97 | single-part, **28 channels** float, ZIPS |
| `ARRI_LogC4-to-Gamma24_Rec709-D65_v1-65.cube` | 9 MB | `LUT_3D_SIZE 65`, no DOMAIN lines |
| `config.ocio` | 8 KB | `ocio_profile_version: 2`, `name: Redshift_3_0` |

**Four things in there change the design, and all four would have been found late.**

### 1. Nothing is tagged. There is no colour space to auto-detect.

Neither EXR carries a `chromaticities` attribute or a `colorSpace` attribute.
The ACEScg-ness of the Redshift render exists only in the owner's head and in
the config — **not in the file**. So "auto-detect common EXR metadata where
possible" resolves to: there is nothing to detect in the material we actually
have. Design for an explicit, remembered choice; treat detection as a bonus that
fires for other people's files.

Worse, the obvious API call gives the **wrong** answer. `config.ocio`'s file
rules read:

```
- !<Rule> {name: exr, extension: exr, pattern: "*", colorspace: Raw}
```

So `Config::getColorSpaceFromFilepath()` on any of these EXRs returns **`Raw`**,
and the picture will look flat and wrong while every API call looks correct.
**The default input colour space must come from the `scene_linear` role** —
which in this config is `ACEScg`, the right answer — and not from the file
rules. Write that decision down in the code, because the file rules are the
thing a reader will reach for first.

### 2. Redshift names its channels `.red` / `.green` / `.blue`

The multilayer file's 27 channels group as:

```
(root)             R      green? no -- R G B          <- the composite
Beauty             red green blue
Cryptomatte        red green blue
DiffuseFilter      red green blue
GI                 red green blue
P                  red green blue
Reflections        red green blue
Shadows            red green blue
SpecularLighting   red green blue
```

The root layer uses `R G B`; **every named layer uses `red green blue`**. Any
layer grouper written against the usual `layer.R` convention will find zero
layers in this file and report it as a plain RGB image. Group case-insensitively
on a suffix set of `{R, red, G, green, B, blue, A, alpha}` and keep the raw
channel names for the HUD so a mismatch is visible rather than silent.

Note also that `(root)` and `Beauty` coexist and are probably identical — the
cycle should not show the same picture twice under two names without saying so.

### 3. `P` is world position and will look like garbage

`P` is not display-referred and is not 0-1. So is a depth or normal pass in other
renders. The owner's brief already anticipates this: non-colour passes need "a
sensible display mapping so they are actually visible". Decide the mapping per
pass *class*, not per pass name, and show which mapping is active — a viewer
that silently normalises is a viewer you cannot trust for review.

### 4. DWAA is lossy, and PIZ is slow

`icecream_passes` is **DWAA** compressed — lossy, by design, at the renderer's
choice. Worth surfacing in the inspector so nobody debugs a compression artefact
as a Trace bug. And the plain sequence is **PIZ**, which is CPU-expensive to
decode: 217 frames at 1920x1080 half RGB is ~1.7 GB on disk and ~6 MB/frame
decoded, i.e. ~150 MB/s at 24fps before any colour work. **EXR playback is a
different streaming problem from H.264 and should be measured before it is
promised.** The existing known gap — "multi-Gbps plates will not stream cold
from LucidLink" — is the same problem wearing a different hat.

## The one architectural decision that matters

**Do not build a LUT feature and then an OCIO feature. OCIO *is* the LUT
loader.** A `.cube` is an OCIO `FileTransform`; it needs no config, and OCIO's
`FileTransform` also reads `.3dl`, `.csp`, `.spi1d`, `.spi3d`, `.clf` and `.ctf`.

That collapses the owner's requirement — "the menu/API structure should not need
to be redesigned when OCIO/ACES support arrives" — into something stronger than
sharing a stage: **there is only ever one stage, and it is OCIO from the first
commit.** "Load LUT…" is that stage holding a `FileTransform`. "Color
Transform…" is that stage holding a `DisplayViewTransform`. The bypass toggle is
the same switch either way. Nothing gets rewritten later because nothing
temporary gets written now.

The state to model is exactly the owner's:

```
enabled (bypass)  +  active transform config  =  displayed result
```

where the config is a small tagged union — `None | FileTransform(path) |
DisplayView(config, input, display, view)` — and everything downstream sees only
a compiled `OCIO::Processor`.

## Dependency reality

vcpkg is pinned to `17f35ad2418007a895ced8a4cece4ab34068a58d`. Checked directly:
that tree carries **OpenColorIO 2.5.2** and **OpenImageIO 3.1.x**.

**That 2.5.2 matters more than it looks.** OCIO 2.5.2 fixes CVE-2026-42450 —
stack buffer overflows from unsafe `sscanf` in the parsers for `.spi3d`,
`.spi1d`, **`.cube`** and `.lut`, affecting all prior 1.x and 2.x. This feature
is precisely "let the user open an arbitrary `.cube`". The current pin is safe;
**the pin must never move backwards**, and that is now a security constraint and
not only a reproducibility one.

Other findings that save a build cycle each:

- **OpenEXR is a hard, non-optional dependency of OIIO 3.x.** There is no
  `openexr` feature to request. `vcpkg install openimageio` already yields a
  working EXR reader.
- Request **`openimageio[opencolorio]`**. OCIO is linked either way, but the
  feature is what makes the exported CMake config emit
  `find_dependency(OpenColorIO)`.
- **Do not enable OIIO's `viewer` feature** — it pulls vcpkg's `qtbase`, which is
  **6.11.1**, a different Qt from the 6.11.2 we ship.
- **Do not enable OIIO's `ffmpeg` feature** — it requests a different feature set
  from the FFmpeg we already pin and would force a rebuild of it.
- Link OCIO directly (`find_package(OpenColorIO CONFIG)`) as well as OIIO. We
  want the OCIO API, not OIIO's `ColorConfig` wrapper.
- `fmt` adds `/utf-8` as a **PUBLIC** MSVC compile option, so linking OIIO adds
  it to our own translation units. Harmless in all likelihood; not invisible.
- OIIO is the heavy item in the graph. The vcpkg binary cache is what makes this
  a one-time cost rather than a per-build one.

## The GPU question, and its real risk

OCIO can emit HLSL: `GPU_LANGUAGE_HLSL_SM_5_0` (the older `GPU_LANGUAGE_HLSL_DX11`
is a deprecated alias for the same value). It emits `Texture3D` +
`SamplerState` declarations named `<texture>Sampler`, and a
`float4 OCIOMain(float4 inPixel)` we concatenate ahead of our own pixel shader.

**The risk is not whether it compiles, it is whether it is right.** OCIO's own
GPU unit tests exercise GLSL and Metal only — there is **no HLSL coverage in
OCIO's test suite**, and OCIO ships no D3D11 helper (the reference implementation
is GLSL). So the HLSL path is generated but never numerically validated
upstream, and we would be writing the resource-binding half ourselves.

**Therefore: CPU first, GPU later, and the GPU stage does not land without a
CPU-vs-GPU comparison harness.** `CPUProcessor` is SIMD-accelerated in this build
and correctness is the thing under test in the first pass. Applying the transform
into a separate display buffer before upload is not a readback and does not
foreclose the GPU stage — it is the same stage, on the other side of the wire.

## Answers to the owner's eight questions, as far as they can honestly be answered now

1. **Architecture** — one OCIO-backed display-transform stage, above. Source
   pixels are never modified; the transform writes a separate display buffer.
2. **Changed files** — expected: `app/CMakeLists.txt`, `src/core/StillImageLoader.*`,
   `src/core/SequenceParser.*` (EXR is not visibly in the sequence path yet and
   must be checked), a new `src/core/ColorTransform.*`, `src/app/MainWindow.cpp`
   (View menu, actions, persistence), `src/app/ShortcutTable.cpp`,
   `src/render/*` for the eventual GPU stage. To be confirmed, not promised.
3. **View menu** — exactly as specified: `Color Transform` (checkable bypass),
   `Color Transform…`, `Load LUT…`, `Reset Color Transform`. Loading a LUT makes
   it active AND enables the toggle in one action.
4. **LUT formats this pass** — `.cube` is the target. `.3dl`, `.csp`, `.spi1d`,
   `.spi3d`, `.clf`, `.ctf` come free with `FileTransform`; offer them only once
   each has been opened at least once.
5. **Persistence** — last transform config persists. A missing LUT or config on
   reopen must fall back to bypass, must not block the media from opening, and
   must say so once.
6. **Copy Frame** — today `copyCurrentFrame()` copies `viewer_->frame()`, i.e.
   whatever the viewer holds. **If the transform is applied into that buffer,
   Copy Frame silently becomes "copies the transformed image".** That is exactly
   the silent change the brief forbids, so it is a decision to be taken
   deliberately and stated in the release notes either way.
7. **Regression** — the existing playback/scrub matrix, unchanged, plus proof
   that transform on/off costs nothing when off.
8. **What remains for OCIO/ACES** — the `Color Transform…` dialog
   (config/input/display/view/look), and the GPU stage.

## Shortcuts

- **`C` is free.** The only `Key_C` binding is `Ctrl+C` (Copy Frame). Bare keys
  currently taken: `E F J K L S T H Space Left Right Home End Esc F11`.
- For pass cycling, **`[` and `]` are free** and give forward *and* back, which a
  single key cannot. The brief asked for "cycle down"; two keys cost nothing and
  a one-way cycle through nine passes is a nuisance to use.

## Staging

Each stage is separately shippable and separately judgeable.

0. **Dependencies only.** Build OIIO + OCIO, prove the existing suite is
   unmoved. EXR-as-a-still already exists in `StillImageLoader` behind
   `TRACE_WITH_OIIO` — this stage may reveal that a single-layer EXR already
   opens once the library is present. Confirm whether the *sequence* path
   accepts EXR; that is the 217-frame test asset and it is not obviously wired.
1. **Color Transform, complete, OCIO-backed, LUT-first.** The whole View menu,
   the bypass, persistence, `C`. Test asset is the **Alexa MOV + `.cube`** —
   note this lands entirely on the *video* path, so the feature earns its keep
   before EXR is finished.
2. **Layer / AOV cycling.** `[` `]`, transient overlay naming the pass, the full
   list in the menu. Display mapping for non-colour passes.
3. **The `Color Transform…` dialog** — config, input, display, view. ACEScg via
   the `scene_linear` role. Now the Redshift config is under test.
4. **Cryptomatte.** The file carries `cryptomatte/be359d6/{name, hash=MurmurHash3_32,
   conversion=uint32_to_float32, manifest}` with `CryptoMaterial` plus ranked
   pairs `CryptoMaterial00…05`. Basic false-colour ID display first.
5. **GPU stage**, with the CPU-vs-GPU comparison harness as its entry condition.

The Alexa MOV is deliberately in stage 1 rather than set aside: it is the
cheapest possible proof that the transform stage is media-agnostic, and it is a
1.5 GB ProRes file, so it doubles as a streaming test.

## Scope guardrails, restated because they are easy to erode

No exposure, gamma or colour-wheel controls. No node graph. No per-shot grade
stack. Trace is a review player with correct colour, not a grading tool. The
first thing that will be asked for after this ships is an exposure slider; the
answer is no.
