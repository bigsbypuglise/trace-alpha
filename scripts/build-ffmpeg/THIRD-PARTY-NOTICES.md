# Third-party notices — FFmpeg

Trace links FFmpeg **dynamically** and ships it as separate DLLs beside `Trace.exe`.
Nothing here is statically linked into Trace, and Trace contains no FFmpeg source.

## What is shipped

| DLL | Library |
|---|---|
| `avcodec-62.dll` | libavcodec |
| `avformat-62.dll` | libavformat |
| `avutil-60.dll` | libavutil |
| `swresample-6.dll` | libswresample |
| `swscale-9.dll` | libswscale |

Built from **FFmpeg n8.1.2** by `scripts/build-ffmpeg/build-minimal-ffmpeg.ps1`,
which pins the source archive, the compiler and the assembler by URL and SHA256.

## Licence

The libraries report **`LGPL version 2.1 or later`** — confirmed at runtime from
`avutil_license()` and `avcodec_license()` on the built binaries, not assumed from
the configure line.

The build passes neither `--enable-gpl` nor `--enable-nonfree` nor
`--enable-version3`, and `--disable-autodetect` guarantees nothing on the build
host can be picked up silently. **Zero external libraries are enabled** — verified
by there being no `--enable-lib*` token in `avcodec_configuration()`. So the
notice obligation is FFmpeg's own LGPL v2.1 and nothing else.

This is the **same licence class the project already ships**: the vcpkg
dependency it replaces is also LGPL v2.1-or-later with all external libraries
disabled. The replacement does not widen the licensing surface.

> It is *narrower* than the prebuilt BtbN `win64-lgpl-shared` artifact that was
> used as the performance control, which is LGPL **v3** and statically links
> libaom, dav1d, vulkan/shaderc and others — each carrying its own notice, for
> code Trace never calls. That is one of the two reasons it was not shipped; the
> other is that it takes the DLL set from 17.3 MB to 104.3 MB.

## LGPL obligations, and how this build meets them

1. **Dynamic linking.** The DLLs are separate files; a user may replace them with
   their own build of the same soname. This is what satisfies the relinking
   requirement without shipping Trace's object files.
2. **Written offer / source availability.** The exact source is the pinned
   archive named in `build-minimal-ffmpeg.ps1` (FFmpeg `n8.1.2`, SHA256 recorded
   there), unmodified. Ship a copy of `COPYING.LGPLv2.1` and `LICENSE.md` from
   that archive alongside the release, and state the version and the configure
   line.
3. **No modifications.** The build applies no patches. The configure flags are
   listed in the script and are readable at runtime from
   `avcodec_configuration()`, so a recipient can reproduce the binary.

## Files to include in the release ZIP

From the FFmpeg source archive, next to the DLLs:

- `COPYING.LGPLv2.1`
- `LICENSE.md`

and this file, which records the version and configure line.
