cmake_minimum_required(VERSION 3.21)
project(sumiyomi VERSION 1.0.1 LANGUAGES C CXX)

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

# Lua 5.4 (design doc §6), compiled as C++ so Lua errors unwind through our host functions'
# destructors instead of longjmp-ing past them. Only the sandbox-safe libraries are built:
# no io, os, package/loadlib, or debug — they can't be reached because they don't exist.
set(LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
set(LUA_SOURCES lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
                llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c lstrlib.c ltable.c
                ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c)
list(TRANSFORM LUA_SOURCES PREPEND "${LUA_DIR}/")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua SYSTEM PUBLIC "${LUA_DIR}")
target_compile_options(lua PRIVATE -w)

# lexbor: HTML5 parser + CSS selectors (design doc §6.1), static.
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/lexbor EXCLUDE_FROM_ALL SYSTEM)

# Image decoding (design doc §7.1), static. zlib + libpng compile straight from source (their
# CMake builds bring tests, installers and shared variants we don't want). libpng's ARM NEON
# filter code stays off: PNG pages are rare (WeebCentral serves JPEG) and it complicates the build.
file(GLOB ZLIB_SOURCES "${CMAKE_SOURCE_DIR}/third_party/zlib/*.c")
add_library(zlib_static STATIC ${ZLIB_SOURCES})
target_include_directories(zlib_static SYSTEM PUBLIC third_party/zlib)
target_compile_definitions(zlib_static PRIVATE HAVE_UNISTD_H)
target_compile_options(zlib_static PRIVATE -w)

set(PNG_DIR "${CMAKE_SOURCE_DIR}/third_party/libpng")
set(PNG_GEN "${CMAKE_BINARY_DIR}/libpng")
configure_file("${PNG_DIR}/scripts/pnglibconf.h.prebuilt" "${PNG_GEN}/pnglibconf.h" COPYONLY)
set(PNG_SOURCES png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c pngrtran.c pngrutil.c pngset.c
                pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c)
list(TRANSFORM PNG_SOURCES PREPEND "${PNG_DIR}/")
add_library(png_static STATIC ${PNG_SOURCES})
target_include_directories(png_static SYSTEM PUBLIC "${PNG_DIR}" "${PNG_GEN}")
target_compile_definitions(png_static PUBLIC PNG_ARM_NEON_OPT=0)
target_compile_options(png_static PRIVATE -w)
target_link_libraries(png_static PUBLIC zlib_static)

# libjpeg-turbo refuses add_subdirectory(); build it as an external project with the same toolchain
# (and so the same -mcpu/-mfpu flags). SIMD on: NEON on the Kindle, NEON/ASIMD on Apple silicon.
include(ExternalProject)
set(JPEG_PREFIX "${CMAKE_BINARY_DIR}/libjpeg-turbo")
set(JPEG_LIB "${JPEG_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(JPEG_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${JPEG_PREFIX} -DCMAKE_INSTALL_LIBDIR=lib
              -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
              -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND JPEG_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()
if(CMAKE_OSX_ARCHITECTURES)
  list(APPEND JPEG_ARGS -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
endif()
ExternalProject_Add(libjpeg_turbo_ext
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libjpeg-turbo"
  PREFIX "${JPEG_PREFIX}/build"
  CMAKE_ARGS ${JPEG_ARGS}
  BUILD_BYPRODUCTS "${JPEG_LIB}"
  UPDATE_COMMAND "" )
file(MAKE_DIRECTORY "${JPEG_PREFIX}/include")
add_library(jpeg_static STATIC IMPORTED GLOBAL)
set_target_properties(jpeg_static PROPERTIES IMPORTED_LOCATION "${JPEG_LIB}"
                                             INTERFACE_INCLUDE_DIRECTORIES "${JPEG_PREFIX}/include")
add_dependencies(jpeg_static libjpeg_turbo_ext)

# libwebp, decoder only: several sources (Mangapill, Bato) serve WebP covers and pages. Its own
# CMake build is well behaved with add_subdirectory as long as every tool and the muxer are off.
# SIMD stays on: NEON on the Kindle is what makes WebP pages affordable there.
foreach(opt WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP
            WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(third_party/libwebp EXCLUDE_FROM_ALL SYSTEM)
# Its targets don't publish an include directory of their own (webp/decode.h lives under src/), and
# CMake won't let us attach one to a target defined in that subdirectory, so carry it separately.
add_library(webp_decode INTERFACE)
target_include_directories(webp_decode SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/libwebp/src")
target_link_libraries(webp_decode INTERFACE webpdecoder)

set(SUMI_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets")

# Backend-independent core: builds for host and device.
add_library(sumi_core STATIC
  src/core/log.cpp
  src/core/canvas.cpp
  src/core/dirty.cpp
  src/core/gesture.cpp
  src/core/refresh_policy.cpp
  src/core/frame.cpp
  src/core/worker.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(sumi_core PRIVATE src/core/loop_linux.cpp)
else()
  target_sources(sumi_core PRIVATE src/core/loop_posix.cpp)
endif()
target_include_directories(sumi_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(sumi_core PUBLIC sumi_warnings Threads::Threads)

# Text: fonts, glyph cache, shaping/layout/drawing (M2 S3).
add_library(sumi_text STATIC
  src/text/fonts.cpp
  src/text/glyph_cache.cpp
  src/text/text.cpp)
target_link_libraries(sumi_text PUBLIC sumi_core harfbuzz)

# UI: tone scale, node tree + layout, screen host (M2 S4).
# Image pipeline (M4): decode, process, cache.
add_library(sumi_image STATIC src/image/decode.cpp src/image/process.cpp src/image/page_cache.cpp)
target_link_libraries(sumi_image PUBLIC sumi_core PRIVATE jpeg_static png_static webp_decode)

add_executable(page_bench tools/image/page_bench.cpp)
target_link_libraries(page_bench PRIVATE sumi_image)
set_target_properties(page_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(sumi_ui STATIC
  src/ui/node.cpp
  src/ui/screen.cpp
  src/ui/widgets.cpp
  src/ui/paged_list.cpp
  src/ui/keyboard.cpp)
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
  src/net/fetch_pool.cpp
  src/net/curl_transport.cpp)
target_link_libraries(sumi_net PUBLIC sumi_core libcurl_static)

# Source extension host: sandboxed Lua + host API + lexbor (M3 S4).
# wasm3: a small WebAssembly interpreter, for Aidoku sources (M6). Builds as C99 for host and Kindle.
set(BUILD_WASI none CACHE STRING "" FORCE)
add_library(wasm3 STATIC)
file(GLOB WASM3_SOURCES "${CMAKE_SOURCE_DIR}/third_party/wasm3/source/*.c")
list(FILTER WASM3_SOURCES EXCLUDE REGEX "m3_api_(wasi|uvwasi|tracer|meta_wasi)\\.c$")
target_sources(wasm3 PRIVATE ${WASM3_SOURCES})
target_include_directories(wasm3 SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/third_party/wasm3/source")
target_compile_definitions(wasm3 PUBLIC d_m3HasWASI=0 d_m3HasTracer=0)
target_compile_options(wasm3 PRIVATE -w)

add_library(sumi_source STATIC
  src/source/html.cpp
  src/source/util.cpp
  src/source/lua_vm.cpp
  src/source/lua_api.cpp
  src/source/extension.cpp
  src/source/repo_index.cpp
  src/source/aix.cpp
  src/source/postcard.cpp
  src/source/aidoku_source.cpp)
target_link_libraries(sumi_source PUBLIC sumi_net lua lexbor_static mbedcrypto zlib_static wasm3)
target_include_directories(sumi_source SYSTEM PRIVATE third_party/lexbor/source)

# App services: data access for the UI over worker jobs (M3 S6).
# One version string for the whole app (user agent, About, the packaged zip).
add_compile_definitions(SUMI_VERSION="${PROJECT_VERSION}")

# Aidoku diagnosis, for a development machine or the device: unpacks a package and runs it end to end.
add_executable(aix_runner tools/aidoku/aix_runner.cpp)
target_include_directories(aix_runner PRIVATE src)
target_link_libraries(aix_runner PRIVATE sumi_source wasm3 lua)
set_target_properties(aix_runner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(CMAKE_CROSSCOMPILING)
  # Static, so the Kindle build also runs under qemu-arm on a development machine (see docs/DEVELOPING.md).
  target_link_options(aix_runner PRIVATE -static)
endif()

add_library(sumi_app_data STATIC
  src/app/app_data.cpp
  src/app/format.cpp)
target_link_libraries(sumi_app_data PUBLIC sumi_data sumi_source sumi_image)

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

  add_executable(test_image_decode tests/unit/test_image_decode.cpp)
  # The test encodes its own WebP images, so it links the full libwebp (encoder included); the app
  # itself only ever links the decoder.
  target_link_libraries(test_image_decode PRIVATE sumi_image jpeg_static png_static webp_decode webp)
  add_test(NAME image_decode COMMAND test_image_decode)

  add_executable(test_image_process tests/unit/test_image_process.cpp)
  target_link_libraries(test_image_process PRIVATE sumi_image)
  add_test(NAME image_process COMMAND test_image_process)

  add_executable(test_page_cache tests/unit/test_page_cache.cpp)
  target_link_libraries(test_page_cache PRIVATE sumi_image)
  add_test(NAME page_cache COMMAND test_page_cache)

  add_executable(test_widgets tests/unit/test_widgets.cpp)
  target_compile_definitions(test_widgets PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}"
                                                 SUMI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  target_link_libraries(test_widgets PRIVATE sumi_ui)
  add_test(NAME widgets COMMAND test_widgets)

  add_executable(test_shell tests/unit/test_shell.cpp src/app/shell.cpp src/app/reader.cpp src/app/sleep_screen.cpp)
  target_include_directories(test_shell PRIVATE src tools/ext)
  target_compile_definitions(test_shell PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}" SUMI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                               SUMI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  target_link_libraries(test_shell PRIVATE sumi_ui sumi_app_data jpeg_static)
  add_test(NAME shell COMMAND test_shell)

  add_executable(test_http tests/unit/test_http.cpp)
  target_compile_definitions(test_http PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(test_http PRIVATE sumi_net)
  add_test(NAME http COMMAND test_http)

  add_executable(http_smoke tools/net/http_smoke.cpp)
  target_compile_definitions(http_smoke PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(http_smoke PRIVATE sumi_net)

  add_executable(test_lua tests/unit/test_lua.cpp)
  target_link_libraries(test_lua PRIVATE sumi_source)
  add_test(NAME lua COMMAND test_lua)

  add_executable(ext_runner tools/ext/ext_runner.cpp)
  target_include_directories(ext_runner PRIVATE tools/ext)
  target_compile_definitions(ext_runner PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}")
  target_link_libraries(ext_runner PRIVATE sumi_source)

  add_executable(test_extension tests/unit/test_extension.cpp)
  target_include_directories(test_extension PRIVATE tools/ext)
  target_compile_definitions(test_extension PRIVATE SUMI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(test_extension PRIVATE sumi_source zlib_static wasm3 lua)
  add_test(NAME extension COMMAND test_extension WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  add_executable(test_app_data tests/unit/test_app_data.cpp)
  target_include_directories(test_app_data PRIVATE tools/ext)
  target_compile_definitions(test_app_data PRIVATE SUMI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(test_app_data PRIVATE sumi_app_data jpeg_static)
  add_test(NAME app_data COMMAND test_app_data)

  add_executable(test_data tests/unit/test_data.cpp)
  target_link_libraries(test_data PRIVATE sumi_data)
  add_test(NAME data COMMAND test_data WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  foreach(name canvas dirty gesture refresh_policy loop frame worker)
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
      src/platform/power_stub.cpp
      src/platform/frontlight_stub.cpp
      src/platform/battery_stub.cpp)
    target_link_libraries(sumi_sim PUBLIC sumi_core SDL2::SDL2)

    add_executable(test_display_sdl tests/unit/test_display_sdl.cpp)
    target_link_libraries(test_display_sdl PRIVATE sumi_sim)
    add_test(NAME display_sdl COMMAND test_display_sdl)

    add_executable(sim_testcard tools/sim/sim_testcard.cpp)
    target_link_libraries(sim_testcard PRIVATE sumi_sim)

    # §2 criterion 7: the same app on the desktop, SDL backend.
    add_library(sumi_sim_platform STATIC src/platform/signals.cpp)
    target_link_libraries(sumi_sim_platform PUBLIC sumi_sim)
    add_executable(sumiyomi src/main.cpp src/app/shell.cpp src/app/reader.cpp src/app/sleep_screen.cpp)
    target_compile_definitions(sumiyomi PRIVATE SUMI_ASSETS_DIR="${SUMI_ASSETS_DIR}"
                                                SUMI_DATA_DIR="${CMAKE_BINARY_DIR}/data"
                                                SUMI_SOURCES_DIR="${CMAKE_SOURCE_DIR}/sources")
    target_link_libraries(sumiyomi PRIVATE sumi_sim_platform sumi_ui sumi_app_data)
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
  src/platform/frontlight_kindle.cpp
  src/platform/battery_kindle.cpp
  src/platform/display_fbink.cpp
  src/platform/input_evdev.cpp)
target_link_libraries(sumi_platform PUBLIC sumi_core fbink)

add_executable(sumiyomi src/main.cpp src/app/shell.cpp src/app/reader.cpp src/app/sleep_screen.cpp)
target_compile_definitions(sumiyomi PRIVATE SUMI_ASSETS_DIR="/mnt/us/extensions/sumiyomi/assets"
                                            SUMI_DATA_DIR="/mnt/us/sumiyomi"
                                            SUMI_SOURCES_DIR="/mnt/us/extensions/sumiyomi/sources")
target_link_libraries(sumiyomi PRIVATE sumi_platform sumi_ui sumi_app_data)

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
