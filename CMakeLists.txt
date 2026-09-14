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

if(CMAKE_CROSSCOMPILING)
  set(SUMI_POWER_SRC src/platform/power_kindle.cpp)
else()
  set(SUMI_POWER_SRC src/platform/power_stub.cpp)
endif()

add_executable(sumiyomi
  src/main.cpp
  src/core/log.cpp
  src/platform/signals.cpp
  ${SUMI_POWER_SRC})
target_include_directories(sumiyomi PRIVATE src)
target_link_libraries(sumiyomi PRIVATE sumi_warnings)
set_target_properties(sumiyomi PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
