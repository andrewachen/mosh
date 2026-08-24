#!/bin/bash
# ABOUTME: Builds the Windows mosh engine dependencies for one target arch.
# ABOUTME: Arch comes from env (TARGET/CROSS_TRIPLE/PREFIX/ARCH); builds zlib,
#          openssl, ncurses, protobuf into $PREFIX and writes .pc files.
set -euo pipefail

: "${TARGET:?TARGET must be set (e.g. aarch64-w64-mingw32)}"
: "${PREFIX:?PREFIX must be set (e.g. /opt/mosh-arm64)}"
: "${ARCH:?ARCH must be set (arm64 or x64)}"

# CMAKE_TOOLCHAIN_FILE is required for the target protobuf build; without it
# cmake would build for the host. Everything else (CC/CXX/AR/AS/LD/RANLIB) is
# exported by the docker stage ENV and used either directly or by the compiler
# drivers.
: "${CMAKE_TOOLCHAIN_FILE:?CMAKE_TOOLCHAIN_FILE must be set}"

# Isolate pkg-config to the target root (PREFIX) so autotools/configure probes
# find only this arch's .pc files, never the host /usr/lib/x86_64-linux-gnu
# ones. SYSROOT_DIR=/ makes -I/-L paths absolute into that root.
export PKG_CONFIG_LIBDIR=${PREFIX}/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/

nproc_all="$(nproc)"

# --- zlib 1.3.1 ---------------------------------------------------------
curl --fail --location --retry 5 --retry-all-errors --output /tmp/zlib.tar.gz \
    https://zlib.net/fossils/zlib-1.3.1.tar.gz
echo '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23  /tmp/zlib.tar.gz' | sha256sum --check
tar -C /tmp -xzf /tmp/zlib.tar.gz
cd /tmp/zlib-1.3.1
CHOST=${TARGET} CC=${TARGET}-clang ./configure --static --prefix=${PREFIX}
make -j"${nproc_all}"
make install
cd /
rm -rf /tmp/zlib-1.3.1 /tmp/zlib.tar.gz

# --- openssl 3.0.16 -------------------------------------------------------
curl --fail --location --retry 5 --retry-all-errors --output /tmp/openssl.tar.gz \
    https://www.openssl.org/source/openssl-3.0.16.tar.gz
echo '57e03c50feab5d31b152af2b764f10379aecd8ee92f16c985983ce4a99f7ef86  /tmp/openssl.tar.gz' | sha256sum --check
tar -C /tmp -xzf /tmp/openssl.tar.gz
cd /tmp/openssl-3.0.16
env -u CC -u CXX -u AR -u AS -u LD -u RANLIB ./Configure mingw64 \
    --cross-compile-prefix=${TARGET}- no-asm no-shared no-tests --prefix=${PREFIX} \
    --libdir=lib
make -j"${nproc_all}" build_libs
make install_dev
cd /
rm -rf /tmp/openssl-3.0.16 /tmp/openssl.tar.gz

# --- ncurses 6.5 ------------------------------------------------------------
curl --fail --location --retry 5 --retry-all-errors --output /tmp/ncurses.tar.gz \
    https://invisible-mirror.net/archives/ncurses/ncurses-6.5.tar.gz
echo '136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6  /tmp/ncurses.tar.gz' | sha256sum --check
tar -C /tmp -xzf /tmp/ncurses.tar.gz
cd /tmp/ncurses-6.5
./configure --host=${TARGET} --prefix=${PREFIX} --without-termlib \
    --with-pkg-config-libdir=${PREFIX}/lib/pkgconfig --without-ada --without-cxx \
    CPPFLAGS='-D__USE_MINGW_ACCESS' --enable-ext-colors \
    --enable-sp-funcs --enable-term-driver --disable-widec --without-debug \
    --without-manpages --without-progs --without-shared --without-tests
make -j"${nproc_all}"
printf '%s\n' '#include <curses.h>' '#include <term.h>' \
    'int main(void) { int errret; return setupterm(0, 1, &errret); }' \
    > tinfo-link-test.c
${TARGET}-gcc -DNCURSES_STATIC tinfo-link-test.c -Iinclude -Llib -lncurses \
    -lws2_32 -luser32 -o tinfo-link-test.exe
make install
cp ${PREFIX}/lib/libncurses.a ${PREFIX}/lib/libtinfo.a
install -m 644 include/ncurses_mingw.h include/nc_mingw.h ${PREFIX}/include/ncurses/
cd /
rm -rf /tmp/ncurses-6.5 /tmp/ncurses.tar.gz

# --- protobuf 3.21.12 (target libs only; host protoc lives in /opt/protobuf-host) ---
curl --fail --location --retry 5 --retry-all-errors --output /tmp/protobuf.tar.gz \
    https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.21.12.tar.gz
echo '930c2c3b5ecc6c9c12615cf5ad93f1cd6e12d0aba862b572e076259970ac3a53  /tmp/protobuf.tar.gz' | sha256sum --check
tar -C /tmp -xzf /tmp/protobuf.tar.gz
cmake -S /tmp/protobuf-3.21.12/cmake -B /tmp/protobuf-target \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${PREFIX} \
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_BUILD_LIBPROTOC=OFF \
    -Dprotobuf_WITH_ZLIB=ON -DZLIB_LIBRARY=${PREFIX}/lib/libz.a \
    -DZLIB_INCLUDE_DIR=${PREFIX}/include
cmake --build /tmp/protobuf-target --parallel "${nproc_all}"
cmake --install /tmp/protobuf-target
rm -rf /tmp/protobuf-3.21.12 /tmp/protobuf.tar.gz /tmp/protobuf-target

# --- .pc files with prefix matching THIS arch's root -------------------------
ln -s ncurses/curses.h ${PREFIX}/include/curses.h
ln -s ncurses/term.h ${PREFIX}/include/term.h
printf '%s\n' \
    "prefix=${PREFIX}" \
    'exec_prefix=${prefix}' \
    'libdir=${prefix}/lib' \
    'includedir=${prefix}/include' \
    '' \
    'Name: tinfo' \
    'Description: ncurses terminal information library' \
    'Version: 6.5' \
    'Libs: -L${libdir} -ltinfo -lws2_32 -luser32' \
    'Cflags: -I${includedir} -DNCURSES_STATIC' \
    > ${PREFIX}/lib/pkgconfig/tinfo.pc
printf '%s\n' \
    "prefix=${PREFIX}" \
    'exec_prefix=${prefix}' \
    'libdir=${prefix}/lib' \
    'includedir=${prefix}/include' \
    '' \
    'Name: protobuf' \
    'Description: Protocol Buffers library' \
    'Version: 3.21.12' \
    'Libs: -L${libdir} -lprotobuf' \
    'Cflags: -I${includedir}' \
    > ${PREFIX}/lib/pkgconfig/protobuf.pc

echo "build-deps.sh: ${ARCH} deps complete at ${PREFIX}"
