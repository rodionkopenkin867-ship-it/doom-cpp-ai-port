# DOOM C++ port — cross-platform build (Windows + Linux).
# macOS is intentionally not supported.
cmake_minimum_required(VERSION 3.16)
project(doom-cpp CXX)

if(APPLE)
    message(FATAL_ERROR "macOS is not supported. Build targets are Windows and Linux only.")
endif()

# The codebase is the original 1993 C translated to C++98; GNU extensions
# (long long in places, __int64 on mingw, etc.) are expected, so keep -gnu++98.
set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Everything portable. Exactly one backend pair per subsystem is added
# per-platform below:
#   i_video.cpp / i_sound.cpp        - original linuxdoom X11/pcspeaker ports (never built)
#   i_video_w32.cpp / i_sound_w32.cpp - Windows GDI + winmm waveOut
#   i_video_sdl.cpp / i_sound_sdl.cpp - Linux SDL2
file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX "src/i_video\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/i_sound\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/i_video_w32\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/i_sound_w32\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/i_video_sdl\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/i_sound_sdl\\.cpp$")

add_executable(doom ${SOURCES})

# The Linux-specific code paths (DOOMWADDIR handling etc.) of the original
# source tree are guarded with NORMALUNIX; the original mingw build used it too.
target_compile_definitions(doom PRIVATE NORMALUNIX)

if(MSVC)
    # The original code relies on GCC-isms (statement expressions style
    # macros, flexible warning levels). MSVC is not exercised.
    message(WARNING "MSVC is not tested for this port; a GCC-style toolchain is recommended.")
else()
    target_compile_options(doom PRIVATE
        -fno-exceptions
        -fno-rtti
        -fpermissive
        # parity with the original zig c++ build (build.ps1): the DOOM source
        # contains decades-old unaligned packed-WAD accesses; the zig/clang
        # runtime alignment+UB checks abort with a panic otherwise.
        -fno-sanitize=undefined
        -fno-sanitize=alignment
    )
    # Same flags at link time so the compiler driver matches the original
    # one-line zig c++ invocation of build.ps1.
    target_link_options(doom PRIVATE
        -fno-sanitize=undefined
        -fno-sanitize=alignment
    )
endif()

if(WIN32)
    # Windows: native GDI video + winmm waveOut sound backends.
    target_sources(doom PRIVATE
        src/i_video_w32.cpp
        src/i_sound_w32.cpp
    )
    target_link_libraries(doom PRIVATE
        user32
        gdi32
        ws2_32
        winmm
    )
elseif(UNIX AND NOT APPLE)
    # Linux: SDL2 video + SDL2 sound backends.
    find_package(SDL2 REQUIRED)
    # Music: MUS lumps are converted to MIDI in memory and played by
    # SDL_mixer. On Debian this is libsdl2-mixer-dev; its CMake config
    # exports the target under different names depending on the version,
    # hence the fallbacks below.
    find_package(SDL2_mixer REQUIRED)
    target_sources(doom PRIVATE
        src/i_video_sdl.cpp
        src/i_sound_sdl.cpp
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(doom PRIVATE SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(doom PRIVATE SDL2::SDL2main)
        endif()
    else()
        target_include_directories(doom PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(doom PRIVATE ${SDL2_LIBRARIES})
    endif()
    if(TARGET SDL2_mixer::SDL2_mixer)           # official sdl2-mixer config
        target_link_libraries(doom PRIVATE SDL2_mixer::SDL2_mixer)
    elseif(TARGET SDL2_mixer::mixer)            # older config name
        target_link_libraries(doom PRIVATE SDL2_mixer::mixer)
    elseif(TARGET SDL2_mixer::SDL2_mixer-static)
        target_link_libraries(doom PRIVATE SDL2_mixer::SDL2_mixer-static)
    else()
        # Fallback: variables from FindSDL2_mixer.cmake-style modules.
        if(SDL2_MIXER_INCLUDE_DIRS)
            target_include_directories(doom PRIVATE ${SDL2_MIXER_INCLUDE_DIRS})
        endif()
        target_link_libraries(doom PRIVATE ${SDL2_MIXER_LIBRARIES})
    endif()
endif()

# Output: doom.exe on Windows, doom on Linux.
set_target_properties(doom PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# CMake places the binary into build/ (or build/<config>/ for multi-config
# generators), while the IWAD lives in the repo root. Copy every *.wad next
# to the executable so a freshly built doom.exe can find it right away.
file(GLOB WAD_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.wad")
if(WAD_FILES)
    add_custom_command(TARGET doom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WAD_FILES}
                $<TARGET_FILE_DIR:doom>
        COMMENT "Copying WAD files next to the doom executable"
        VERBATIM
    )
endif()
