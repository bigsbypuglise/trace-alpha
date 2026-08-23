set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
# Audio output only. Optional so the app still builds and plays picture on a
# toolchain without Qt Multimedia.
find_package(Qt6 QUIET COMPONENTS Multimedia)
# --- EXR + colour management (stage 0) --------------------------------------
#
# OpenImageIO is the EXR reader; OpenColorIO is the colour pipeline. Both are
# QUIET/optional for the same reason FFmpeg and Qt Multimedia are: a toolchain
# without them must still build a working player, just one that declines EXR.
# The configure-time status lines below are what stop that degrading silently --
# a build with no OIIO opens every other format and reports "EXR support not
# enabled" only once someone tries one.
#
# OCIO IS FOUND DIRECTLY, NOT REACHED THROUGH OIIO. OIIO exposes a ColorConfig
# wrapper, but the transform stage this dependency exists for needs the OCIO API
# itself (Config, Processor, CPUProcessor, FileTransform). Requesting the
# openimageio[opencolorio] feature is still what makes OIIO's exported CMake
# config emit find_dependency(OpenColorIO), so the two are complementary rather
# than redundant.
#
# THE VCPKG PIN CARRIES OCIO 2.5.2 AND MUST NOT MOVE BACKWARDS. 2.5.2 fixes
# CVE-2026-42450 -- stack buffer overflows from unsafe sscanf in the .cube,
# .spi1d, .spi3d and .lut parsers, affecting every prior 1.x and 2.x. This whole
# feature is "let the user open an arbitrary .cube", so the pin is a security
# constraint now and not only a reproducibility one.
#
# Two features are deliberately NOT requested. `viewer` pulls vcpkg's qtbase,
# which is 6.11.1 -- a different Qt from the 6.11.2 that ships. `ffmpeg` asks for
# a different FFmpeg feature set from the one pinned here and would rebuild it.
find_package(OpenImageIO CONFIG QUIET)
find_package(OpenColorIO CONFIG QUIET)

# TRACE_FFMPEG_ROOT selects the minimal MinGW/GCC LGPL FFmpeg built by
# scripts/build-ffmpeg/build-minimal-ffmpeg.ps1 instead of whatever the toolchain
# (vcpkg) supplies. Unset by default, so the existing MSVC/vcpkg dependency path
# is untouched and remains available until the replacement has passed every gate.
#
# It exists because the two builds differ by ~18% of decode throughput on large
# ProRes and by nothing else: same avcodec version, same sonames, same feature
# set, bit-identical output. Pointing CMake at a directory is what makes that an
# A/B rather than a migration.
set(TRACE_FFMPEG_ROOT "" CACHE PATH
    "Root of a prebuilt FFmpeg (bin/, include/, lib/) to use instead of the toolchain's")

if(TRACE_FFMPEG_ROOT)
  # THE CACHE IS CLEARED BEFORE SEARCHING, AND THAT IS NOT BELT-AND-BRACES.
  # find_path/find_library write CACHE entries and return immediately when one
  # is already set -- so a build tree first configured WITHOUT this flag keeps
  # whatever it found on the default path (vcpkg) forever, and adding
  # -DTRACE_FFMPEG_ROOT=... to a later configure silently changes nothing.
  #
  # Found on the dev box 2026-08-23: build/ carried
  #   TRACE_FFMPEG_ROOT = C:/tw_ff9/out                     (the flag WAS set)
  #   FFMPEG_AVCODEC_LIBRARY = C:/vcpkg/.../debug/lib/avcodec.lib
  # i.e. it linked vcpkg's MSVC FFmpeg 8.1.2 -- and its DEBUG import library --
  # while printing that it was using the minimal GCC 9.0.1 tree. The exe's
  # import table read avcodec-62.dll against the control's avcodec-63.dll.
  # That is the ~18%-slower decode toolchain, under a status line saying
  # otherwise.
  foreach(_v FFMPEG_INCLUDE_DIR FFMPEG_AVFORMAT_LIBRARY FFMPEG_AVCODEC_LIBRARY
             FFMPEG_AVUTIL_LIBRARY FFMPEG_SWSCALE_LIBRARY FFMPEG_SWRESAMPLE_LIBRARY)
    unset(${_v} CACHE)
  endforeach()

  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
            PATHS "${TRACE_FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
  foreach(_ff avformat avcodec avutil swscale swresample)
    string(TOUPPER ${_ff} _FF)
    find_library(FFMPEG_${_FF}_LIBRARY ${_ff}
                 PATHS "${TRACE_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  endforeach()

  # REPORT THE RESOLVED PATH, NOT THE REQUEST. The old line echoed the flag back,
  # which is a claim that cannot fail -- it printed correctly throughout the fault
  # above. What is printed now is what the linker will actually use, so the two
  # cannot disagree without saying so. Same rule as the HUD's renderer/planar/font
  # fields: a knob whose state cannot be read back is a knob you cannot trust a
  # null result from.
  message(STATUS "Trace: FFmpeg avcodec resolved to ${FFMPEG_AVCODEC_LIBRARY}")
else()
  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h)
  find_library(FFMPEG_AVFORMAT_LIBRARY avformat)
  find_library(FFMPEG_AVCODEC_LIBRARY avcodec)
  find_library(FFMPEG_AVUTIL_LIBRARY avutil)
  find_library(FFMPEG_SWSCALE_LIBRARY swscale)
  find_library(FFMPEG_SWRESAMPLE_LIBRARY swresample)
endif()

set(TRACE_FFMPEG_FOUND FALSE)
if(FFMPEG_INCLUDE_DIR AND FFMPEG_AVFORMAT_LIBRARY AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWSCALE_LIBRARY)
  set(TRACE_FFMPEG_FOUND TRUE)
endif()

# Audio needs all three: a decoder, a resampler, and a device to play out of.
set(TRACE_AUDIO_FOUND FALSE)
if(TRACE_FFMPEG_FOUND AND FFMPEG_SWRESAMPLE_LIBRARY AND Qt6Multimedia_FOUND)
  set(TRACE_AUDIO_FOUND TRUE)
endif()

# --- Native D3D11 presentation (GATE B) -------------------------------------
#
# Windows + MSVC only, and opt-in at runtime via TRACE_RENDERER=d3d11. The
# shaders are compiled HERE with fxc rather than at runtime with D3DCompile:
# d3dcompiler_47.dll is not deployed by windeployqt, so a runtime compile would
# configure green and then fail to create a shader on a clean machine.
#
# fxc ships with the Windows SDK. If it cannot be found the whole backend is
# left out and the app builds exactly as it did before -- a missing tool must
# not be a broken build.
set(TRACE_D3D11_FOUND FALSE)
set(TRACE_D3D11_SOURCES "")

if(WIN32 AND MSVC)
  find_program(TRACE_FXC fxc
    HINTS
      "$ENV{WindowsSdkVerBinPath}/x64"
      "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
      "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  )
  if(NOT TRACE_FXC)
    # Any SDK version will do for vs_4_0/ps_4_0; take the newest present.
    file(GLOB TRACE_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
    if(TRACE_FXC_CANDIDATES)
      list(SORT TRACE_FXC_CANDIDATES)
      list(REVERSE TRACE_FXC_CANDIDATES)
      list(GET TRACE_FXC_CANDIDATES 0 TRACE_FXC)
    endif()
  endif()
endif()

if(TRACE_FXC)
  set(TRACE_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${TRACE_SHADER_DIR}")

  # /Fh emits a C header holding the compiled bytecode as a byte array named by
  # /Vn, so the shaders are linked into the exe and there is no runtime file to
  # find, deploy, or get out of step with the build.
  function(trace_compile_shader src profile entryvar out)
    add_custom_command(
      OUTPUT "${TRACE_SHADER_DIR}/${out}"
      COMMAND "${TRACE_FXC}" /nologo /T ${profile} /E main /Vn ${entryvar}
              /Fh "${TRACE_SHADER_DIR}/${out}" "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
      MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
      COMMENT "fxc ${profile} ${src}"
      VERBATIM)
  endfunction()

  # vs_4_0/ps_4_0: feature level 10.0 and up, which keeps WARP in scope for CI.
  trace_compile_shader(../src/render/shaders/FullscreenQuad.vs.hlsl vs_4_0
                       g_TraceFullscreenQuadVS FullscreenQuad.vs.h)
  trace_compile_shader(../src/render/shaders/TexturedQuad.ps.hlsl ps_4_0
                       g_TraceTexturedQuadPS TexturedQuad.ps.h)
  trace_compile_shader(../src/render/shaders/OverlayQuad.vs.hlsl vs_4_0
                       g_TraceOverlayQuadVS OverlayQuad.vs.h)
  trace_compile_shader(../src/render/shaders/OverlayQuad.ps.hlsl ps_4_0
                       g_TraceOverlayQuadPS OverlayQuad.ps.h)
  trace_compile_shader(../src/render/shaders/YuvToRgb.ps.hlsl ps_4_0
                       g_TraceYuvToRgbPS YuvToRgb.ps.h)

  set(TRACE_D3D11_FOUND TRUE)
  set(TRACE_D3D11_SOURCES
    ../src/render/D3D11VideoRenderer.h
    ../src/render/D3D11VideoRenderer.cpp
    ../src/render/D3D11OverlayDrawer.h
    ../src/render/D3D11OverlayDrawer.cpp
    ../src/render/shaders/FullscreenQuad.vs.hlsl
    ../src/render/shaders/TexturedQuad.ps.hlsl
    ../src/render/shaders/OverlayQuad.vs.hlsl
    ../src/render/shaders/OverlayQuad.ps.hlsl
    ../src/render/shaders/YuvToRgb.ps.hlsl
    "${TRACE_SHADER_DIR}/FullscreenQuad.vs.h"
    "${TRACE_SHADER_DIR}/TexturedQuad.ps.h"
    "${TRACE_SHADER_DIR}/OverlayQuad.vs.h"
    "${TRACE_SHADER_DIR}/OverlayQuad.ps.h"
    "${TRACE_SHADER_DIR}/YuvToRgb.ps.h"
  )
  # .hlsl files are inputs to our own fxc rules; stop MSBuild running its
  # built-in HLSL task over them as well, which picks its own profile and fails.
  set_source_files_properties(
    ../src/render/shaders/FullscreenQuad.vs.hlsl
    ../src/render/shaders/TexturedQuad.ps.hlsl
    ../src/render/shaders/OverlayQuad.vs.hlsl
    ../src/render/shaders/OverlayQuad.ps.hlsl
    ../src/render/shaders/YuvToRgb.ps.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE VS_TOOL_OVERRIDE "None")
endif()

# WIN32: GUI subsystem, so Windows does not create a console on every launch
# (owner item 18, 2026-08-18 -- the console was never a portable-ZIP artifact,
# it was this missing keyword). Qt6::Core supplies the WinMain shim through
# Qt6::EntryPointPrivate when WIN32_EXECUTABLE is set, so main() is unchanged.
# The TRACE_*_LOG stderr knobs survive via AttachConsole in main.cpp -- a GUI
# process has no console of its own to write to.
add_executable(Trace WIN32
  ../src/main.cpp
  ../src/app/MainWindow.h
  ../src/app/MainWindow.cpp
  ../src/app/ShortcutTable.h
  ../src/app/ShortcutTable.cpp
  ../src/app/MediaShare.h
  ../src/app/MediaShare.cpp
  ../src/app/Settings.h
  ../src/app/Settings.cpp
  ../src/app/Theme.cpp
  ../src/app/RecentFiles.h
  ../src/app/RecentFiles.cpp
  ../src/app/MovieInspector.h
  ../src/app/MovieInspector.cpp
  ../src/app/ShortcutsWindow.h
  ../src/app/ShortcutsWindow.cpp
  ../src/app/OverlayAccessibility.h
  ../src/app/OverlayAccessibility.cpp
  ../src/app/WindowShape.h
  ../src/app/WindowShape.cpp
  ../src/app/LucidLinkIntegration.h
  ../src/app/LucidLinkIntegration.cpp
  ../src/ui/ViewerWidget.h
  ../src/ui/ViewerWidget.cpp
  ../src/ui/OverlaySpike.h
  ../src/ui/OverlaySpike.cpp
  ../src/render/OverlayHooks.h
  # Backend-neutral, so it is listed here rather than in TRACE_D3D11_SOURCES:
  # the CPU renderer draws the same overlay and must build without D3D11.
  ../src/render/OverlayModel.h
  ../src/render/OverlayModel.cpp
  ../src/render/ViewTransform.h
  ../src/render/ViewScale.h
  ../src/render/VideoRenderer.h
  ../src/render/VideoRenderer.cpp
  ../src/render/CpuImageRenderer.h
  ../src/render/CpuImageRenderer.cpp
  ${TRACE_D3D11_SOURCES}
  ../src/ui/TransportOverlay.h
  ../src/ui/TransportOverlay.cpp
  ../src/ui/TransportBar.h
  ../src/ui/TransportBar.cpp
  ../src/ui/TopChrome.h
  ../src/ui/TopChrome.cpp
  ../src/core/MediaItem.h
  ../src/core/PlaybackState.h
  ../src/core/ViewState.h
  ../src/core/PlaybackController.h
  ../src/core/PlaybackController.cpp
  ../src/core/ColorTransform.h
  ../src/core/ColorTransform.cpp
  ../src/core/StillImageLoader.h
  ../src/core/StillImageLoader.cpp
  ../src/core/SequenceDescriptor.h
  ../src/core/SequenceParser.h
  ../src/core/SequenceParser.cpp
  ../src/core/TimeFormat.h
  ../src/core/TimeFormat.cpp
  ../src/core/FrameCache.h
  ../src/core/FrameCache.cpp
  ../src/core/VideoDecoderFFmpeg.h
  ../src/core/VideoDecoderFFmpeg.cpp
  ../src/core/AudioOutput.h
  ../src/core/AudioOutput.cpp
  ../src/core/MediaIoSource.h
  ../src/core/MediaIoSource.cpp
  ../src/core/VideoFrame.h
  ../src/core/VideoFrame.cpp
  ../src/core/FrameSource.h
  ../src/core/VideoFrameSource.h
  ../src/core/VideoFrameSource.cpp
  ../src/core/ScrubDecodeWorker.h
  ../src/core/ScrubDecodeWorker.cpp
  ../src/core/ImageSequenceFrameSource.h
  ../src/core/ImageSequenceFrameSource.cpp
  resources.qrc
)

if(WIN32)
  # trace.manifest: the Windows 8+ supportedOS declaration, merged by the
  # linker's mt step into its default manifest. Layered CHILD windows --
  # which the TopChrome fade (owner item 11) depends on -- are silently
  # ignored without it; see the comment in the manifest itself.
  target_sources(Trace PRIVATE trace.rc trace.manifest)
endif()

target_include_directories(Trace PRIVATE ../src)
target_link_libraries(Trace PRIVATE Qt6::Widgets)

# Spec phase 14: About Trace and the Report an Issue mail body both have to say
# which build the user is running. The version lives in the top-level project()
# call and nothing read it until now -- so it is passed as a definition rather
# than typed a second time in C++, where the two would drift the first time one
# of them was bumped.
target_compile_definitions(Trace PRIVATE
  TRACE_VERSION_STRING="${PROJECT_VERSION}")

if(WIN32)
  # Spec phase 8: SHOpenFolderAndSelectItems / SHParseDisplayName for Show in
  # File Explorer. The shell interface rather than `explorer.exe /select,<path>`
  # -- there is no process-argument quoting of that command line that works for
  # every path, and the spec's phase 9 rule (invoke through shell interfaces,
  # never screen automation) is the same house style one phase early.
  target_link_libraries(Trace PRIVATE shell32 ole32)
endif()

if(TRACE_D3D11_FOUND)
  # The generated shader headers live in the build tree.
  target_include_directories(Trace PRIVATE "${TRACE_SHADER_DIR}")
  target_link_libraries(Trace PRIVATE d3d11 dxgi)
  target_compile_definitions(Trace PRIVATE TRACE_WITH_D3D11=1)
  message(STATUS "Trace: D3D11 renderer enabled (opt-in via TRACE_RENDERER=d3d11)")
else()
  message(STATUS "Trace: D3D11 renderer DISABLED (needs Windows + MSVC + fxc)")
endif()

if(OpenImageIO_FOUND)
  target_link_libraries(Trace PRIVATE OpenImageIO::OpenImageIO)
  target_compile_definitions(Trace PRIVATE TRACE_WITH_OIIO=1)
  message(STATUS "Trace: OpenImageIO enabled (EXR reader) ${OpenImageIO_VERSION}")
else()
  message(STATUS "Trace: OpenImageIO DISABLED -- EXR will not open")
endif()

if(OpenColorIO_FOUND)
  target_link_libraries(Trace PRIVATE OpenColorIO::OpenColorIO)
  target_compile_definitions(Trace PRIVATE TRACE_WITH_OCIO=1)
  message(STATUS "Trace: OpenColorIO enabled ${OpenColorIO_VERSION}")
else()
  message(STATUS "Trace: OpenColorIO DISABLED -- no colour transform stage")
endif()

if(TRACE_FFMPEG_FOUND)
  target_include_directories(Trace PRIVATE ${FFMPEG_INCLUDE_DIR})
  target_link_libraries(Trace PRIVATE
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
  )
  target_compile_definitions(Trace PRIVATE TRACE_WITH_FFMPEG=1)
endif()

if(TRACE_AUDIO_FOUND)
  target_link_libraries(Trace PRIVATE
    Qt6::Multimedia
    ${FFMPEG_SWRESAMPLE_LIBRARY}
  )
  target_compile_definitions(Trace PRIVATE TRACE_WITH_AUDIO=1)
  message(STATUS "Trace: audio output enabled")
else()
  message(STATUS "Trace: audio output DISABLED (needs Qt6 Multimedia + swresample)")
endif()
