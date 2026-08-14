# decbench -- decode-only throughput against the FFmpeg Trace actually links

The ceiling on what any Trace pipeline could present. No conversion, no upload, no
render, no seek: it reads the file sequentially once per configuration and decodes
every frame. One persistent decoder per configuration over the whole file; nothing
is reopened between samples within a run.

**Build it against vcpkg, not against whatever `ffmpeg.exe` is on PATH.** That
substitution is what made the first 8K measurement wrong by 23%: the winget
`ffmpeg` is a GCC build and Trace links an MSVC one, and on this file they differ
by more than any tuning inside Trace does.

MSBuild's FileTracker cannot cope with a long scratch path, so build it directly:

```
cl /nologo /O2 /std:c++20 /EHsc /MD main.cpp \
   /I C:\vcpkg\installed\x64-windows\include \
   /Fe:decbench.exe \
   /link /LIBPATH:C:\vcpkg\installed\x64-windows\lib avcodec.lib avformat.lib avutil.lib
```

Copy the `av*.dll` / `sw*.dll` from `build\app\Release` beside it, then:

```
decbench.exe <file> <source fps>
```

It prints the stream's exact identity (codec, profile, pixel format, bit depth,
plane count, chroma log2, alpha) and the decoder's advertised threading
capabilities before the sweep, so a result can never be read against the wrong
file or the wrong decoder.

**It reports p50/p95/max per frame, not a mean.** Frame threading on the 8K plate
reads p50 8.4ms and max 883ms; an average describes neither.
