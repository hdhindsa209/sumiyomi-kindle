cmake_minimum_required(VERSION 3.21)
project(sumiyomi VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# §12 coding conventions.
add_library(sumi_warnings INTERFACE)
target_compile_options(sumi_warnings INTERFACE
  -Wall -Wextra -Wshadow -Wconversion -Werror
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# Third-party text stack (design doc §3.2): FreeType + HarfBuzz, static, for host and device.
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL SYSTEM)

# HarfBuzz as its single-file amalgamation, with the FreeType integration (hb-ft).
add_library(harfbuzz STATIC third_party/harfbuzz/src/harfbuzz.cc)
target_compile_definitions(harfbuzz PUBLIC HAVE_FREETYPE=1)
target_include_directories(harfbuzz SYSTEM PUBLIC third_party/harfbuzz/src)
target_compile_options(harfbuzz PRIVATE -fno-exceptions -fno-rtti -w)
target_link_libraries(harfbuzz PUBLIC freetype)

set(SUMI_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")

# Backend-independent core: builds for host and device.
add_library(sumi_core STATIC
  src/core/log.cpp
  src/core/canvas.cpp
  src/core/dirty.cpp
  src/core/gesture.cpp
  src/core/refresh_policy.cpp
  src/core/frame.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(sumi_core PRIVATE src/core/loop_linux.cpp)
else()
  target_sources(sumi_core PRIVATE src/core/loop_posix.cpp)
endif()
target_include_directories(sumi_core PUBLIC src)
target_link_libraries(sumi_core PUBLIC sumi_warnings)

if(NOT CMAKE_CROSSCOMPILING)
  # Host: unit tests, plus the SDL simulator backend (§10) when SDL2 is available.
  enable_testing()
  add_executable(test_text_smoke tests/unit/test_text_smoke.cpp)
  target_include_directories(test_text_smoke PRIVATE assets/fonts)
  target_compile_definitions(test_text_smoke PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(test_text_smoke PRIVATE sumi_warnings harfbuzz)
  add_test(NAME text_smoke COMMAND test_text_smoke)

  foreach(name canvas dirty gesture refresh_policy loop frame)
    add_executable(test_${name} tests/unit/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE sumi_core)
    if(name STREQUAL "loop")
      find_package(Threads REQUIRED)
      target_link_libraries(test_${name} PRIVATE Threads::Threads)
    endif()
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()

  find_package(SDL2 CONFIG)
  if(SDL2_FOUND)
    add_library(sumi_sim STATIC
      src/platform/display_sdl.cpp
      src/platform/input_sdl.cpp
      src/platform/power_stub.cpp)
    target_link_libraries(sumi_sim PUBLIC sumi_core SDL2::SDL2)

    add_executable(test_display_sdl tests/unit/test_display_sdl.cpp)
    target_link_libraries(test_display_sdl PRIVATE sumi_sim)
    add_test(NAME display_sdl COMMAND test_display_sdl)

    add_executable(sim_testcard tools/sim/sim_testcard.cpp)
    target_link_libraries(sim_testcard PRIVATE sumi_sim)

    # §2 criterion 7: the same app on the desktop, SDL backend.
    add_library(sumi_sim_platform STATIC src/platform/signals.cpp)
    target_link_libraries(sumi_sim_platform PUBLIC sumi_sim)
    add_executable(sumiyomi src/main.cpp src/app/m1_testcard.cpp)
    target_link_libraries(sumiyomi PRIVATE sumi_sim_platform)
  else()
    message(STATUS "SDL2 not found: simulator not built (brew install sdl2)")
  endif()
  return()
endif()

# FBInk: built with its own Makefile (§3.3), consumed as an imported static library.
set(FBINK_DIR "${CMAKE_SOURCE_DIR}/third_party/fbink")
set(FBINK_LIB "${FBINK_DIR}/Release/libfbink.a")
add_custom_target(fbink_build
  COMMAND make -j4 staticlib CROSS_TC=arm-kindlehf-linux-gnueabihf KINDLE=true
  WORKING_DIRECTORY "${FBINK_DIR}"
  BYPRODUCTS "${FBINK_LIB}"
  USES_TERMINAL)
add_library(fbink STATIC IMPORTED GLOBAL)
set_target_properties(fbink PROPERTIES
  IMPORTED_LOCATION             "${FBINK_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${FBINK_DIR}"
  INTERFACE_LINK_LIBRARIES      m)
add_dependencies(fbink fbink_build)

add_library(sumi_platform STATIC
  src/platform/signals.cpp
  src/platform/power_kindle.cpp
  src/platform/display_fbink.cpp
  src/platform/input_evdev.cpp)
target_link_libraries(sumi_platform PUBLIC sumi_core fbink)

add_executable(sumiyomi src/main.cpp src/app/m1_testcard.cpp)
target_link_libraries(sumiyomi PRIVATE sumi_platform)

add_executable(m1_bench tools/bench/m1_bench.cpp)
target_link_libraries(m1_bench PRIVATE sumi_platform)

# Device-side S2 smoke test: SUMI_ASSETS=/path/to/assets ./text_smoke
add_executable(text_smoke tests/unit/test_text_smoke.cpp)
target_include_directories(text_smoke PRIVATE assets/fonts tests/unit)
target_compile_definitions(text_smoke PRIVATE SUMI_ASSETS_DIR="/mnt/us/extensions/sumiyomi/assets")
target_link_libraries(text_smoke PRIVATE sumi_warnings harfbuzz)

set_target_properties(sumiyomi m1_bench text_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
