# The native build (macOS, Linux): the game's C++ for the host with src/platform/native/ as its platform layer.
#
#   cmake -S native -B build/native/cmake -G Ninja && cmake --build build/native/cmake
#
# The retail data comes from the split as C++ (native/tools/convert_data.py writes build/native/data.cpp; this build runs it).
# See native/NATIVE.md.
cmake_minimum_required(VERSION 3.24)
project(TwinsanityNative VERSION 1.0.0 LANGUAGES CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# C++26 like the PS2 build's gnu++26 (CMake doesn't know AppleClang has it and would fall back to C++23)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Line tables only: Apple clang 21 with full debug info (-g) drops the asm labels (RETAIL) of explicit specializations' members
# (game/pools.h's), whose callers then don't link. File and line in backtraces is what debugging the port needs
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -gline-tables-only -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The movies' MPEG-2 pictures (src/platform/native/movie/): FFmpeg's libavcodec and libavutil. Our own LGPL 2.1, decode-only
# build (native/tools/build_ffmpeg.sh, in build/native/ffmpeg/<os>-<arch>) when it's there, else the system's pkg-config one
# (Homebrew's is GPL 3: an app carrying it can't be shared under the LGPL). See native/third_party/ffmpeg/README.md
find_package(PkgConfig REQUIRED)
if(APPLE)
    set(TWIN_FFMPEG_OS macos)
else()
    string(TOLOWER ${CMAKE_SYSTEM_NAME} TWIN_FFMPEG_OS)
endif()
list(LENGTH CMAKE_OSX_ARCHITECTURES TWIN_ARCHITECTURES)
if(TWIN_ARCHITECTURES EQUAL 1)
    set(TWIN_FFMPEG_ARCH ${CMAKE_OSX_ARCHITECTURES})
else()
    set(TWIN_FFMPEG_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(TWIN_FFMPEG_ARCH STREQUAL "aarch64")
    set(TWIN_FFMPEG_ARCH arm64)
endif()
set(TWIN_FFMPEG_DIR ${ROOT}/build/native/ffmpeg/${TWIN_FFMPEG_OS}-${TWIN_FFMPEG_ARCH} CACHE PATH
    "The FFmpeg build to link (native/tools/build_ffmpeg.sh's install folder); the system's when it has no lib/pkgconfig")
if(EXISTS ${TWIN_FFMPEG_DIR}/lib/pkgconfig/libavcodec.pc)
    set(TWIN_FFMPEG_CHOICE ${TWIN_FFMPEG_DIR})
    # Only ours: pkg-config looks nowhere else
    set(ENV{PKG_CONFIG_LIBDIR} ${TWIN_FFMPEG_DIR}/lib/pkgconfig)
    set(ENV{PKG_CONFIG_PATH} "")
else()
    set(TWIN_FFMPEG_CHOICE system)
    message(WARNING "FFmpeg: no LGPL build in ${TWIN_FFMPEG_DIR} (native/tools/build_ffmpeg.sh makes it); using the "
        "system's, which may be GPL (Homebrew's is GPL 3, with x264 and x265): fine to play, not to share the app")
endif()
# pkg-config's answer and the libraries' paths are cached: found again when the choice changed
if(NOT "${TWIN_FFMPEG_CHOSEN}" STREQUAL "${TWIN_FFMPEG_CHOICE}")
    set(FFMPEG_FOUND "" CACHE INTERNAL "")
    get_cmake_property(TWIN_CACHED CACHE_VARIABLES)
    foreach(variable ${TWIN_CACHED})
        if(variable MATCHES "^pkgcfg_lib_FFMPEG_")
            unset(${variable} CACHE)
        endif()
    endforeach()
    set(TWIN_FFMPEG_CHOSEN ${TWIN_FFMPEG_CHOICE} CACHE INTERNAL "")
endif()
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL libavcodec libavutil)
message(STATUS "FFmpeg: ${TWIN_FFMPEG_CHOICE} (libavcodec ${FFMPEG_libavcodec_VERSION})")

# The game's C++, and the decomp's own C++ of the retail C library and maths (portable, kept for their exact results)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${ROOT}/src/game/*.cpp)
set(CORE_SOURCES ${ROOT}/src/debug.cpp ${ROOT}/src/gcc2.cpp ${ROOT}/src/main.cpp)
file(STRINGS ${ROOT}/native/reused_ps2.txt REUSED_PS2 REGEX "^src/")
list(TRANSFORM REUSED_PS2 PREPEND ${ROOT}/)
set(RETAIL_SOURCES ${REUSED_PS2})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROOT}/native/reused_ps2.txt)
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS ${ROOT}/src/platform/native/*.cpp)

# The retail data as C++, made again when the split, the converter or the C++'s declarations change
set(DATA_CPP ${ROOT}/build/native/data.cpp)
file(GLOB DATA_INPUTS CONFIGURE_DEPENDS ${ROOT}/asm/data/*.s ${ROOT}/include/*.h ${ROOT}/include/*/*.h)
add_custom_command(
    OUTPUT ${DATA_CPP}
    COMMAND ${Python3_EXECUTABLE} native/tools/data_types.py
    COMMAND ${Python3_EXECUTABLE} native/tools/convert_data.py
    WORKING_DIRECTORY ${ROOT}
    DEPENDS ${ROOT}/native/tools/data_types.py ${ROOT}/native/tools/convert_data.py ${DATA_INPUTS} ${GAME_SOURCES}
            ${CORE_SOURCES} ${RETAIL_SOURCES} ${PLATFORM_SOURCES} ${ROOT}/native/reused_ps2.txt
    COMMENT "Converting the retail data")

# Everything compiled once into objects; the stand-ins for what the platform layer doesn't do yet (weak, native/tools/
# make_stubs.py) are made from what the objects leave undefined, then the program is linked
# The script commands' arguments as the files have them (native/tools/script_layouts.py)
set(SCRIPT_LAYOUTS_CPP ${ROOT}/build/native/scriptlayouts.cpp)
add_custom_command(
    OUTPUT ${SCRIPT_LAYOUTS_CPP}
    COMMAND ${Python3_EXECUTABLE} native/tools/script_layouts.py
    WORKING_DIRECTORY ${ROOT}
    DEPENDS ${ROOT}/native/tools/script_layouts.py ${ROOT}/native/tools/convert_data.py ${DATA_CPP}
    COMMENT "Laying out the script commands' arguments")

# VU1's programs translated to C++ from the game's microcode (native/tools/vu1_translate.py; native/GRAPHICS.md, "VU1
# translation")
set(VU1_PROGRAMS_CPP ${ROOT}/build/native/vu1programs.cpp)
foreach(shard RANGE 7)
    list(APPEND VU1_PROGRAMS_CPP ${ROOT}/build/native/vu1programs_${shard}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${VU1_PROGRAMS_CPP}
    COMMAND ${Python3_EXECUTABLE} native/tools/vu1_translate.py ${ROOT}/build/native/vu1programs.cpp
    WORKING_DIRECTORY ${ROOT}
    DEPENDS ${ROOT}/native/tools/vu1_translate.py ${ROOT}/native/tools/vudecode.py ${ROOT}/assets/vutext.textbin.bin
            ${ROOT}/asm/data/vudata.data.s ${ROOT}/src/platform/native/graphics/renderer/vuprograms.cpp
    COMMENT "Translating VU1's programs")
# VU0's microprogram sets the same way (vu1_translate.py --vu0)
set(VU0_PROGRAMS_CPP ${ROOT}/build/native/vu0programs.cpp ${ROOT}/build/native/vu0programs_0.cpp ${ROOT}/build/native/vu0programs_1.cpp)
add_custom_command(
    OUTPUT ${VU0_PROGRAMS_CPP}
    COMMAND ${Python3_EXECUTABLE} native/tools/vu1_translate.py --vu0 ${ROOT}/build/native/vu0programs.cpp
    WORKING_DIRECTORY ${ROOT}
    DEPENDS ${ROOT}/native/tools/vu1_translate.py ${ROOT}/native/tools/vudecode.py ${ROOT}/assets/vutext.textbin.bin
    COMMENT "Translating VU0's programs")

# The files defining explicit specializations' members keep line tables only in every build (see above)
file(GLOB SPECIALIZING_SOURCES ${ROOT}/src/game/pools.cpp)
foreach(source ${GAME_SOURCES} ${CORE_SOURCES})
    file(STRINGS ${source} specializes REGEX "^template <>")
    if(specializes)
        set_source_files_properties(${source} PROPERTIES COMPILE_OPTIONS -gline-tables-only)
    endif()
endforeach()

add_library(game OBJECT ${GAME_SOURCES} ${CORE_SOURCES} ${RETAIL_SOURCES} ${PLATFORM_SOURCES} ${DATA_CPP} ${SCRIPT_LAYOUTS_CPP}
            ${VU1_PROGRAMS_CPP} ${VU0_PROGRAMS_CPP})
set(STUBS_CPP ${ROOT}/build/native/stubs.cpp)
add_custom_command(
    OUTPUT ${STUBS_CPP}
    COMMAND ${Python3_EXECUTABLE} native/tools/make_stubs.py ${STUBS_CPP} $<TARGET_OBJECTS:game>
    WORKING_DIRECTORY ${ROOT}
    DEPENDS game $<TARGET_OBJECTS:game> ${ROOT}/native/tools/make_stubs.py
    COMMAND_EXPAND_LISTS
    COMMENT "Making the stand-ins for what the platform layer doesn't do yet")
add_executable(twinsanity $<TARGET_OBJECTS:game> ${STUBS_CPP})

foreach(target game twinsanity)
target_include_directories(${target} PRIVATE ${ROOT}/native/include ${ROOT}/include ${ROOT}/src/platform/native)
target_compile_definitions(${target} PRIVATE TWIN_NATIVE)
# What the PS2 build's flags keep of the retail code's behaviour: no fused multiply-adds (the R5900 has none the game's code
# uses), no assumptions about aliasing, null checks or signed overflow the retail compiler didn't make
target_compile_options(${target} PRIVATE
    -std=gnu++2c -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-strict-aliasing -fno-delete-null-pointer-checks -fwrapv
    -ffp-contract=off -Wall -Wno-invalid-offsetof -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
    -Wno-unknown-pragmas -Wno-missing-braces -Wno-mismatched-tags)
target_link_libraries(${target} PRIVATE SDL3::SDL3 OpenGL::GL PkgConfig::FFMPEG)
endforeach()
if(APPLE)
    # A Mac app: "Crash Twinsanity.app", with the icon native/tools/make_icon.py makes from the game's own art when it's there
    set(ICON ${ROOT}/build/native/icon/AppIcon.icns)
    set_target_properties(twinsanity PROPERTIES
        OUTPUT_NAME "Crash Twinsanity"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "Crash Twinsanity"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.twinsanity.native"
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE AppIcon)
    if(EXISTS ${ICON})
        target_sources(twinsanity PRIVATE ${ICON})
        set_source_files_properties(${ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    # The UI's fonts with their licences (native/third_party/fonts/README.md), in Resources/fonts
    set(UI_FONTS ${ROOT}/native/third_party/fonts/LuckiestGuy-Regular.ttf ${ROOT}/native/third_party/fonts/Fredoka-SemiBold.ttf
        ${ROOT}/native/third_party/fonts/Fredoka-Bold.ttf ${ROOT}/native/third_party/fonts/LICENSE-LuckiestGuy.txt
        ${ROOT}/native/third_party/fonts/OFL-Fredoka.txt ${ROOT}/native/third_party/fonts/README.md)
    target_sources(twinsanity PRIVATE ${UI_FONTS})
    set_source_files_properties(${UI_FONTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
endif()

# The release: the app built, made standalone (its libraries in Contents/Frameworks, their licences, signed for this Mac) and zipped:
# build/native/dist/Crash Twinsanity.app and its .zip. `cmake --build build/native/cmake --target release`
if(APPLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(release
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/native/tools/bundle_app.py --app $<TARGET_BUNDLE_DIR:twinsanity> --zip
        DEPENDS twinsanity
        WORKING_DIRECTORY ${ROOT}
        COMMENT "Making the standalone Crash Twinsanity.app (build/native/dist)"
        VERBATIM)
endif()
