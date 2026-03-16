set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenImageIO QUIET)

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h)
find_library(FFMPEG_AVFORMAT_LIBRARY avformat)
find_library(FFMPEG_AVCODEC_LIBRARY avcodec)
find_library(FFMPEG_AVUTIL_LIBRARY avutil)
find_library(FFMPEG_SWSCALE_LIBRARY swscale)

set(TRACE_FFMPEG_FOUND FALSE)
if(FFMPEG_INCLUDE_DIR AND FFMPEG_AVFORMAT_LIBRARY AND FFMPEG_AVCODEC_LIBRARY AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWSCALE_LIBRARY)
  set(TRACE_FFMPEG_FOUND TRUE)
endif()

add_executable(Trace
  ../src/main.cpp
  ../src/app/MainWindow.h
  ../src/app/MainWindow.cpp
  ../src/ui/ViewerWidget.h
  ../src/ui/ViewerWidget.cpp
  ../src/ui/TransportOverlay.h
  ../src/ui/TransportOverlay.cpp
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
  ../src/core/FrameSource.h
  ../src/core/VideoFrameSource.h
  ../src/core/VideoFrameSource.cpp
  ../src/core/ImageSequenceFrameSource.h
  ../src/core/ImageSequenceFrameSource.cpp
  ../src/core/PlaybackPrefetcher.h
  ../src/core/PlaybackPrefetcher.cpp
  resources.qrc
)

target_include_directories(Trace PRIVATE ../src)
target_link_libraries(Trace PRIVATE Qt6::Widgets)

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
