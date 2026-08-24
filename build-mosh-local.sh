#!/bin/bash
# ABOUTME: Locally cross-compile the Windows mosh engine (arm64 or x64) in Docker.
# ABOUTME: MOSH_ARCH selects the driver, prefix, pkgconfig, and flags; runs the
# ABOUTME: dependency smoke test and the Milestone 0 standalone engine build.

set -e

# Source tree to build. Defaults to the primary mosh checkout; override with the
# MOSH_LOCAL env var to build a git worktree (e.g. a per-milestone worktree).
MOSH_LOCAL="${MOSH_LOCAL:-/home/achen/git/gh/mosh}"

# Select the target architecture: driver trio, install prefix, and CPU flags.
MOSH_ARCH="${MOSH_ARCH:-arm64}"
case "$MOSH_ARCH" in
  arm64)
    DRIVER=aarch64-w64-mingw32
    PREFIX=/opt/mosh-arm64
    WIN_MCPU=-mcpu=oryon-1
    ;;
  x64)
    DRIVER=x86_64-w64-mingw32
    PREFIX=/opt/mosh-x64
    WIN_MCPU=-march=meteorlake
    ;;
  *)
    echo "error: unknown MOSH_ARCH '$MOSH_ARCH' (expected arm64 or x64)" >&2
    exit 1
    ;;
esac

# --user 1000:1000 preserves file ownership for generated mosh build output.
log=$(mktemp)
trap 'rm -f "$log"' EXIT

# The in-container payload stays single-quoted so $scratch, $(mktemp -d),
# $(nproc), and the trap remain literal; arch values arrive as container env
# vars (MOSH_DRIVER/MOSH_PREFIX/WIN_MCPU) rather than interpolation.
if docker run --rm \
  --user 1000:1000 \
  -e MOSH_DRIVER="$DRIVER" -e MOSH_PREFIX="$PREFIX" \
  -e WIN_MCPU="$WIN_MCPU" -e PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
  -v "$PWD:/work" \
  -v "$MOSH_LOCAL:/mosh" \
  -w /work \
  mosh-win \
  bash -c 'set -e
    scratch=$(mktemp -d)
    trap "rm -rf \"$scratch\"" EXIT
    cat > "$scratch/win32-smoke.cc" <<"EOF"
#include <openssl/aes.h>
#include <google/protobuf/message.h>
#include <zlib.h>
#include <curses.h>
#include <term.h>

int main() {
  AES_KEY key;
  const int key_status = AES_set_encrypt_key(nullptr, 128, &key);
  google::protobuf::ShutdownProtobufLibrary();
  const uLong bound = compressBound(0);
  // Reference setupterm — the exact tinfo entry point used by mosh
  // terminaldisplayinit.cc — so the linker must resolve it (and the MinGW
  // tty-shim symbols it pulls in) from libtinfo.a. The ERR macro or a
  // lighter function would mask an incomplete tinfo archive.
  int tinfo_errret = 0;
  const int tinfo_status = setupterm(nullptr, 1, &tinfo_errret);
  return key_status + static_cast<int>(bound) + tinfo_status + tinfo_errret;
}
EOF
    "${MOSH_DRIVER}-clang++" -DNCURSES_STATIC "$scratch/win32-smoke.cc" \
      -o "$scratch/win32-smoke.exe" -I"$MOSH_PREFIX/include" \
      -L"$MOSH_PREFIX/lib" -lcrypto -lprotobuf -lz -ltinfo -lws2_32 -luser32
    cd /mosh
    make -f win32/Makefile.win clean
    make -j"$(nproc)" -f win32/Makefile.win CXX="${MOSH_DRIVER}-clang++" AR="${MOSH_DRIVER}-ar" NM="${MOSH_DRIVER}-nm" OBJDUMP="${MOSH_DRIVER}-objdump" WIN_MCPU="$WIN_MCPU" PREFIX="$MOSH_PREFIX" check' >"$log" 2>&1; then
  status=0
else
  status=$?
fi

grep -E 'error:|fatal error:' "$log" | \
  grep -v '^make' | \
  sort -u || true
exit "$status"
