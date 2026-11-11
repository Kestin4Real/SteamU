cmake_minimum_required(VERSION 3.20)
project(SteamU VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
include(ProcessorCount)
ProcessorCount(NPROC)

# ---------------------------------------------------------------------------
# Shared code (protocol, networking, logging)
# ---------------------------------------------------------------------------
add_library(steamu_common STATIC
    src/common/log.cpp
    src/common/net.cpp
)
target_include_directories(steamu_common PUBLIC src)

# ---------------------------------------------------------------------------
# PC side: virtual Steam Deck controller + screen capture/encode
# Uses the system FFmpeg (Ubuntu/Mint builds include NVENC).
# ---------------------------------------------------------------------------
pkg_check_modules(HOST_DEPS REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale x11 xext xrandr libpulse-simple opus)

add_executable(steamu-host
    src/host/main.cpp
    src/host/uhid_device.cpp
    src/host/usb_identity.cpp
    src/host/usbip_device.cpp
    src/host/x11_capture.cpp
    src/host/video_streamer.cpp
    src/host/screen_server.cpp
    src/host/obs_output.cpp
    src/host/audio_streamer.cpp
)
target_include_directories(steamu-host PRIVATE sdk)
target_link_libraries(steamu-host PRIVATE steamu_common PkgConfig::HOST_DEPS)

# ---------------------------------------------------------------------------
# Minimal static FFmpeg for the Deck: HEVC/H.264 decoders + VAAPI, and the
# Opus decoder (which needs libswresample) for audio.
# Static, so nothing has to be installed on SteamOS; libva comes from the
# system (SteamOS ships it with Mesa).
# ---------------------------------------------------------------------------
include(ExternalProject)
set(FFMPEG_VERSION 7.1.1)
set(FFMPEG_LOCAL_TARBALL ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-${FFMPEG_VERSION}.tar.xz)
if(EXISTS ${FFMPEG_LOCAL_TARBALL})
    set(FFMPEG_URL ${FFMPEG_LOCAL_TARBALL})
else()
    set(FFMPEG_URL https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz)
endif()
set(FFMPEG_PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-deck)
file(MAKE_DIRECTORY ${FFMPEG_PREFIX}/include)

ExternalProject_Add(ffmpeg_deck
    URL ${FFMPEG_URL}
    URL_HASH SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=${FFMPEG_PREFIX}
        --enable-static --disable-shared --enable-pic
        --disable-everything --disable-autodetect --disable-programs --disable-doc
        --disable-network --disable-x86asm
        --disable-avdevice --disable-avformat --disable-avfilter
        --disable-swscale
        --enable-vaapi
        --enable-decoder=hevc,h264,opus
        --enable-parser=hevc,h264
        --enable-hwaccel=hevc_vaapi,h264_vaapi
    BUILD_COMMAND make -j${NPROC}
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${FFMPEG_PREFIX}/lib/libavcodec.a ${FFMPEG_PREFIX}/lib/libswresample.a
        ${FFMPEG_PREFIX}/lib/libavutil.a
)

pkg_check_modules(VA REQUIRED IMPORTED_TARGET libva libva-drm)
add_library(deck_ffmpeg INTERFACE)
add_dependencies(deck_ffmpeg ffmpeg_deck)
target_include_directories(deck_ffmpeg INTERFACE ${FFMPEG_PREFIX}/include)
target_link_libraries(deck_ffmpeg INTERFACE
    ${FFMPEG_PREFIX}/lib/libavcodec.a ${FFMPEG_PREFIX}/lib/libswresample.a
    ${FFMPEG_PREFIX}/lib/libavutil.a
    PkgConfig::VA m pthread)

# ---------------------------------------------------------------------------
# Static SDL3 for the Deck, so steamu-deck is a single file. SDL still loads
# X11/Wayland/GL/Vulkan from the system at runtime (SteamOS has them).
# ---------------------------------------------------------------------------
set(SDL_DECK_VERSION 3.4.0)
set(SDL_DECK_LOCAL_TARBALL ${CMAKE_SOURCE_DIR}/third_party/SDL3-${SDL_DECK_VERSION}.tar.gz)
if(EXISTS ${SDL_DECK_LOCAL_TARBALL})
    set(SDL_DECK_URL ${SDL_DECK_LOCAL_TARBALL})
else()
    set(SDL_DECK_URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL_DECK_VERSION}/SDL3-${SDL_DECK_VERSION}.tar.gz)
endif()
set(SDL_DECK_PREFIX ${CMAKE_BINARY_DIR}/sdl-deck)
file(MAKE_DIRECTORY ${SDL_DECK_PREFIX}/include)

ExternalProject_Add(sdl_deck
    URL ${SDL_DECK_URL}
    URL_HASH SHA256=082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${SDL_DECK_PREFIX}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DSDL_SHARED=OFF -DSDL_STATIC=ON
        -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
        -DSDL_X11_XTEST=OFF
    BUILD_BYPRODUCTS ${SDL_DECK_PREFIX}/lib/libSDL3.a
)

add_library(deck_sdl INTERFACE)
add_dependencies(deck_sdl sdl_deck)
target_include_directories(deck_sdl INTERFACE ${SDL_DECK_PREFIX}/include)
target_link_libraries(deck_sdl INTERFACE ${SDL_DECK_PREFIX}/lib/libSDL3.a dl m pthread rt)

# ---------------------------------------------------------------------------
# Steam Deck side: controller streaming + fullscreen video display.
# Single self-contained binary: static SDL3, FFmpeg and libstdc++; only
# glibc and libva are taken from SteamOS.
# ---------------------------------------------------------------------------
add_executable(steamu-deck
    src/deck/main.cpp
    src/deck/hidraw_backend.cpp
    src/deck/fake_backend.cpp
    src/deck/video_receiver.cpp
    src/deck/audio_receiver.cpp
)
target_link_libraries(steamu-deck PRIVATE steamu_common deck_ffmpeg deck_sdl)
target_link_options(steamu-deck PRIVATE -static-libstdc++ -static-libgcc)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
# Second-screen API test client: draws a pattern and touch points on the Deck.
add_executable(steamu-screentest tools/screentest.c)
target_include_directories(steamu-screentest PRIVATE sdk)
target_link_libraries(steamu-screentest PRIVATE m)

add_executable(steamu-inputtest tools/inputtest.cpp)
# Uses the same static SDL3 as the Deck app (no system SDL3 needed).
target_link_libraries(steamu-inputtest PRIVATE deck_sdl)
