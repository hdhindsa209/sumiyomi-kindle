# Cross-compile for the Kindle via koxtoolchain's `kindlehf` target.
# Device: i.MX6 SoloLite, single-core Cortex-A9, hard-float, FW 5.17.1.0.3.
# See docs/DEVICE_FACTS.md (U1) for where these values came from.

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ENV{KINDLE_TC_ROOT})
  message(FATAL_ERROR "Set KINDLE_TC_ROOT to your x-tools target dir "
                      "(e.g. ~/x-tools/arm-kindlehf-linux-gnueabihf)")
endif()
set(TC_ROOT   $ENV{KINDLE_TC_ROOT})
set(TC_PREFIX "${TC_ROOT}/bin/arm-kindlehf-linux-gnueabihf-")

set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++")
set(CMAKE_AR           "${TC_PREFIX}ar")
set(CMAKE_RANLIB       "${TC_PREFIX}ranlib")
set(CMAKE_STRIP        "${TC_PREFIX}strip")

set(CMAKE_SYSROOT "${TC_ROOT}/arm-kindlehf-linux-gnueabihf/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# /proc/cpuinfo: CPU part 0xc09 (Cortex-A9), Features: neon vfpv3 vfpd32.
# Differs from the M1 spec's cortex-a53/neon-vfpv4 example — that would SIGILL here.
set(SUMI_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT   "${SUMI_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SUMI_ARCH_FLAGS}")

# Static-link libstdc++/libgcc: the Kindle's runtime (glibc 2.20) is too old to rely on.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
