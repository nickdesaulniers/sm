# Game Boy Advance build of the game. (The PC build is the Makefile.)
#   cmake -B build-gba -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-gba
# Produces build-gba/sm-engine.gba, the engine, and from it and sm.smc (if
# there), build-gba/sm.gba, the cartridge: see tools/make_cartridge.py.
cmake_minimum_required(VERSION 3.20)
project(sm_gba LANGUAGES C ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The GBA build requires Clang. Found: ${CMAKE_C_COMPILER_ID}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# Without the ROM (as in CI), the build makes only the engine; see
# tools/make_cartridge.py.
set(SM_ROM "${CMAKE_SOURCE_DIR}/sm.smc" CACHE FILEPATH
    "The Super Metroid ROM to make sm.gba with (sha1 da957f0d63d14cb441d215462904c4fa8519c613)")
if(NOT EXISTS "${SM_ROM}")
    message(STATUS "SM_ROM: no ${SM_ROM}: building only sm-engine.gba")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The code is not compiled -ffreestanding: that implies -fno-builtin, which
# stops clang inlining memcpy/memset and similar. Instead it links -nostdlib
# and gba/libc provides the few library functions needed; only that code is
# built with -fno-builtin. The game casts pointers between types freely, so
# no strict aliasing (the PC build doesn't either).
add_compile_options(${GBA_COMPILE_OPTIONS} -fno-strict-aliasing)
add_link_options(${GBA_LINK_OPTIONS})
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/gba/libc/include)
include(cmake/compiler-rt.cmake)

add_subdirectory(gba)
