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
find_package(OpenImageIO QUIET)

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h)
find_library(FFMPEG_AVFORMAT_LIBRARY avformat)
find_library(FFMPEG_AVCODEC_LIBRARY avcodec)
find_library(FFMPEG_AVUTIL_LIBRARY avutil)
find_library(FFMPEG_SWSCALE_LIBRARY swscale)
find_library(FFMPEG_SWRESAMPLE_LIBRARY swresample)

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

add_executable(Trace
  ../src/main.cpp
  ../src/app/MainWindow.h
  ../src/app/MainWindow.cpp
  ../src/app/ShortcutTable.h
  ../src/app/ShortcutTable.cpp
  ../src/app/MediaShare.h
  ../src/app/MediaShare.cpp
  ../src/app/Settings.h
  ../src/app/Settings.cpp
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
  ../src/render/VideoRenderer.h
  ../src/render/VideoRenderer.cpp
  ../src/render/CpuImageRenderer.h
  ../src/render/CpuImageRenderer.cpp
  ${TRACE_D3D11_SOURCES}
  ../src/ui/TransportOverlay.h
  ../src/ui/TransportOverlay.cpp
  ../src/ui/TransportBar.h
  ../src/ui/TransportBar.cpp
  ../src/core/MediaItem.h
  ../src/core/PlaybackState.h
  ../src/core/ViewState.h
  ../src/core/PlaybackController.h
  ../src/core/PlaybackController.cpp
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
  target_sources(Trace PRIVATE trace.rc)
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
