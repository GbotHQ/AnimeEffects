project(anime_media)

# The media library owns all FFmpeg linkage. It is deliberately GL-free (and
# mostly QCoreApplication-free) so it can be unit-tested headless; it only
# needs Qt Core for QString/QStringList.
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(PkgConfig REQUIRED)

# Link the standard FFmpeg libav* modules. On Fedora, pkg-config resolves
# these to the LGPL "free" build; on other distros / our CI (where we build
# FFmpeg from source) pkg-config points at whatever is installed. Keep this
# portable — the GPL encoder support is handled by the override below.
pkg_check_modules(LIBFMT    REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL    REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE   REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMP  REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(AVCODEC_PC REQUIRED IMPORTED_TARGET libavcodec)

# GPL encoder support override. On systems where pkg-config's libavcodec is
# the LGPL "free" build, it does NOT contain the GPL x264/x265 encoders which
# the H.264/H.265 export presets require. To get them in a dev build, point
# AE_FFMPEG_GPL_AVCODEC at a GPL libavcodec (e.g. Fedora's
# /usr/lib64/ffmpeg/libavcodec.so). Leave it empty to use pkg-config's lib.
# In CI (Phase 7) we build FFmpeg from source (GPL) and point this at it so the
# encoder set is deterministic.
set(AE_FFMPEG_GPL_AVCODEC "" CACHE FILEPATH
    "Path to a GPL libavcodec.so (with libx264/libx265). Empty = pkg-config's libavcodec.")

if(AE_FFMPEG_GPL_AVCODEC AND EXISTS "${AE_FFMPEG_GPL_AVCODEC}")
    # Import the GPL build directly so the app links (and RUNPATH-loads) it.
    add_library(FFmpeg::avcodec UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::avcodec PROPERTIES
        IMPORTED_LOCATION "${AE_FFMPEG_GPL_AVCODEC}"
        INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_PC_INCLUDE_DIRS}"
        IMPORTED_NO_SONAME TRUE
    )
else()
    add_library(FFmpeg::avcodec INTERFACE IMPORTED)
    set_target_properties(FFmpeg::avcodec PROPERTIES
        INTERFACE_LINK_LIBRARIES PkgConfig::AVCODEC_PC
        INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_PC_INCLUDE_DIRS}"
    )
endif()

add_library(media STATIC
    FFmpegHelpers.cpp
    FFmpegHelpers.h
    VideoEncoder.cpp
    VideoEncoder.h
    AudioMixer.cpp
    AudioMixer.h
)
target_include_directories(media PUBLIC
    # Resolve "media/FFmpegHelpers.h" the same way the app resolves
    # "ctrl/Exporter.h": from src/.
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(media PUBLIC
    Qt6::Core
    Qt6::Gui
    PkgConfig::LIBFMT PkgConfig::AVUTIL PkgConfig::SWSCALE PkgConfig::SWRESAMP
    FFmpeg::avcodec
)
