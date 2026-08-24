# ABOUTME: CMake cross toolchain for the llvm-mingw toolchain (aarch64 or x86_64).
# ABOUTME: Derived from the dockcross windows-arm64 Toolchain.cmake; used by the
#          protobuf target build in Dockerfile.mosh-win.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 1)

set(cross_triple $ENV{CROSS_TRIPLE})
set(cross_root $ENV{CROSS_ROOT})

# Derive the target processor from the per-stage triple rather than hard-coding
# it: the same Toolchain.cmake serves both the arm64 and x64 dep stages.
if(cross_triple MATCHES "^x86_64-")
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
elseif(cross_triple MATCHES "^aarch64-")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
else()
  message(FATAL_ERROR "Unsupported CROSS_TRIPLE '${cross_triple}'")
endif()

set(CMAKE_C_COMPILER $ENV{CC})
set(CMAKE_CXX_COMPILER $ENV{CXX})
set(CMAKE_Fortran_COMPILER $ENV{FC})

set(CMAKE_CXX_FLAGS "-I ${cross_root}/include/")

list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH} ${cross_root} ${cross_root}/${cross_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# No CMAKE_CROSSCOMPILING_EMULATOR: the outputs are Windows PE binaries, which
# qemu cannot run (it emulates Linux ELF CPUs, not a Windows userspace).
# Execution testing happens on the matching-arch Windows CI runners (arm64 and
# x64).
