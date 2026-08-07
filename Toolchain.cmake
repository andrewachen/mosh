# ABOUTME: CMake cross toolchain for the aarch64-w64-mingw32 llvm-mingw toolchain.
# ABOUTME: Derived from the dockcross windows-arm64 Toolchain.cmake; used by the
#          protobuf target build in Dockerfile.mosh-arm64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(cross_triple $ENV{CROSS_TRIPLE})
set(cross_root $ENV{CROSS_ROOT})

set(CMAKE_C_COMPILER $ENV{CC})
set(CMAKE_CXX_COMPILER $ENV{CXX})
set(CMAKE_Fortran_COMPILER $ENV{FC})

set(CMAKE_CXX_FLAGS "-I ${cross_root}/include/")

list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH} ${cross_root} ${cross_root}/${cross_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# No CMAKE_CROSSCOMPILING_EMULATOR: the outputs are Windows PE binaries, which
# qemu-aarch64 cannot run (it emulates a CPU for Linux ELF, not a Windows
# userspace). Execution testing happens on the windows-11-arm CI runner.
