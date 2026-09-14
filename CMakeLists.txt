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

if(NOT CMAKE_CROSSCOMPILING)
  # The app needs a display backend; the host one is SDL (T11). Until then the
  # host preset only builds host-side tests (T07/T08).
  message(STATUS "Host build: sumiyomi app skipped until the SDL backend lands (T11)")
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
  src/core/log.cpp
  src/platform/signals.cpp
  src/platform/power_kindle.cpp
  src/platform/display_fbink.cpp)
target_include_directories(sumi_platform PUBLIC src)
target_link_libraries(sumi_platform PUBLIC sumi_warnings fbink)

add_executable(sumiyomi src/main.cpp)
target_link_libraries(sumiyomi PRIVATE sumi_platform)

add_executable(m1_bench tools/bench/m1_bench.cpp)
target_link_libraries(m1_bench PRIVATE sumi_platform)

set_target_properties(sumiyomi m1_bench PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
