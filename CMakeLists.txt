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

# SQLite amalgamation (design doc §3.2), static.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PUBLIC
  SQLITE_THREADSAFE=2               # connections are never shared across threads
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_USE_ALLOCA
  SQLITE_ENABLE_FTS5)
target_compile_options(sqlite3 PRIVATE -w)

# Network (design doc §9): mbedTLS 3.6 LTS + libcurl, static, HTTP(S) only. The Kindle's system
# OpenSSL is too old for modern TLS, so the TLS stack and CA bundle ship with the app.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL SYSTEM)

# Point curl's FindMbedTLS at the in-tree targets instead of searching the system.
set(MBEDTLS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/mbedtls/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
foreach(opt HTTP_ONLY CURL_USE_MBEDTLS CURL_DISABLE_INSTALL)
  set(${opt} ON CACHE BOOL "" FORCE)
endforeach()
foreach(opt BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_TESTING BUILD_EXAMPLES BUILD_LIBCURL_DOCS BUILD_MISC_DOCS
            ENABLE_CURL_MANUAL CURL_USE_OPENSSL CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_GSSAPI USE_NGHTTP2
            USE_LIBIDN2 USE_APPLE_IDN ENABLE_UNIX_SOCKETS CURL_BROTLI CURL_ZSTD CURL_USE_PKGCONFIG)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_CA_BUNDLE none CACHE STRING "" FORCE)   # we always pass CURLOPT_CAINFO
set(CURL_CA_PATH none CACHE STRING "" FORCE)
add_subdirectory(third_party/curl EXCLUDE_FROM_ALL SYSTEM)

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

# Text: fonts, glyph cache, shaping/layout/drawing (M2 S3).
add_library(sumi_text STATIC
  src/text/fonts.cpp
  src/text/glyph_cache.cpp
  src/text/text.cpp)
target_link_libraries(sumi_text PUBLIC sumi_core harfbuzz)

# UI: tone scale, node tree + layout, screen host (M2 S4).
add_library(sumi_ui STATIC
  src/ui/node.cpp
  src/ui/screen.cpp
  src/ui/widgets.cpp
  src/ui/paged_list.cpp)
target_include_directories(sumi_ui PUBLIC assets/fonts)
target_link_libraries(sumi_ui PUBLIC sumi_text)

# Data: SQLite repositories (M3 S1).
add_library(sumi_data STATIC
  src/data/db.cpp
  src/data/repo.cpp)
target_link_libraries(sumi_data PUBLIC sumi_core sqlite3)

# Net: HTTP client over libcurl + mbedTLS (M3 S2).
add_library(sumi_net STATIC
  src/net/http.cpp
  src/net/curl_transport.cpp)
target_link_libraries(sumi_net PUBLIC sumi_core libcurl_static)

if(NOT CMAKE_CROSSCOMPILING)
  # Host: unit tests, plus the SDL simulator backend (§10) when SDL2 is available.
  enable_testing()
  add_executable(test_text_smoke tests/unit/test_text_smoke.cpp)
  target_include_directories(test_text_smoke PRIVATE assets/fonts)
  target_compile_definitions(test_text_smoke PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(test_text_smoke PRIVATE sumi_warnings harfbuzz)
  add_test(NAME text_smoke COMMAND test_text_smoke)

  add_executable(test_text tests/unit/test_text.cpp)
  target_include_directories(test_text PRIVATE assets/fonts)
  target_compile_definitions(test_text PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(test_text PRIVATE sumi_text)
  add_test(NAME text COMMAND test_text)

  add_executable(test_ui tests/unit/test_ui.cpp)
  target_compile_definitions(test_ui PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}"
                                             SUMI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  target_link_libraries(test_ui PRIVATE sumi_ui)
  add_test(NAME ui COMMAND test_ui)

  add_executable(test_widgets tests/unit/test_widgets.cpp)
  target_compile_definitions(test_widgets PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}"
                                                 SUMI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  target_link_libraries(test_widgets PRIVATE sumi_ui)
  add_test(NAME widgets COMMAND test_widgets)

  add_executable(test_shell tests/unit/test_shell.cpp src/app/shell.cpp src/app/fake_data.cpp)
  target_include_directories(test_shell PRIVATE src)
  target_compile_definitions(test_shell PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}"
                                               SUMI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  target_link_libraries(test_shell PRIVATE sumi_ui)
  add_test(NAME shell COMMAND test_shell)

  add_executable(test_http tests/unit/test_http.cpp)
  target_compile_definitions(test_http PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(test_http PRIVATE sumi_net)
  add_test(NAME http COMMAND test_http)

  add_executable(http_smoke tools/net/http_smoke.cpp)
  target_compile_definitions(http_smoke PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(http_smoke PRIVATE sumi_net)

  add_executable(test_data tests/unit/test_data.cpp)
  target_link_libraries(test_data PRIVATE sumi_data)
  add_test(NAME data COMMAND test_data WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

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
    add_executable(sumiyomi src/main.cpp src/app/shell.cpp src/app/fake_data.cpp)
    target_compile_definitions(sumiyomi PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
    target_link_libraries(sumiyomi PRIVATE sumi_sim_platform sumi_ui)
    add_executable(m1_testcard tools/m1/testcard_main.cpp src/app/m1_testcard.cpp)
    target_link_libraries(m1_testcard PRIVATE sumi_sim_platform)
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

add_executable(sumiyomi src/main.cpp src/app/shell.cpp src/app/fake_data.cpp)
target_compile_definitions(sumiyomi PRIVATE SUMI_ASSETS_DIR="/mnt/us/extensions/sumiyomi/assets")
target_link_libraries(sumiyomi PRIVATE sumi_platform sumi_ui)

add_executable(m1_testcard tools/m1/testcard_main.cpp src/app/m1_testcard.cpp)
target_link_libraries(m1_testcard PRIVATE sumi_platform)

add_executable(m1_bench tools/bench/m1_bench.cpp)
target_link_libraries(m1_bench PRIVATE sumi_platform)

# Device-side S2 smoke test: SUMI_ASSETS=/path/to/assets ./text_smoke
add_executable(text_smoke tests/unit/test_text_smoke.cpp)
target_include_directories(text_smoke PRIVATE assets/fonts tests/unit)
target_compile_definitions(text_smoke PRIVATE SUMI_ASSETS_DIR="/mnt/us/extensions/sumiyomi/assets")
target_link_libraries(text_smoke PRIVATE sumi_warnings harfbuzz)

# Device-side live TLS check: ./http_smoke [url] [ca_bundle]
add_executable(http_smoke tools/net/http_smoke.cpp)
target_compile_definitions(http_smoke PRIVATE SUMI_ASSETS_DIR="/mnt/us/extensions/sumiyomi/assets")
target_link_libraries(http_smoke PRIVATE sumi_net)

set_target_properties(sumiyomi m1_testcard m1_bench text_smoke http_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
