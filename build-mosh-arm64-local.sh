#!/bin/bash
# ABOUTME: Locally cross-compile the Windows ARM64 mosh engine in Docker.
# ABOUTME: Runs the dependency smoke test and Milestone 0 standalone engine build.

set -e

# Source tree to build. Defaults to the primary mosh checkout; override with the
# MOSH_LOCAL env var to build a git worktree (e.g. a per-milestone worktree).
MOSH_LOCAL="${MOSH_LOCAL:-/home/achen/git/gh/mosh}"

# --user 1000:1000 preserves file ownership for generated mosh build output.
log=$(mktemp)
trap 'rm -f "$log"' EXIT

if docker run --rm \
  --user 1000:1000 \
  -v "$PWD:/work" \
  -v "$MOSH_LOCAL:/mosh" \
  -w /work \
  mosh-arm64 \
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
    aarch64-w64-mingw32-clang++ -DNCURSES_STATIC "$scratch/win32-smoke.cc" \
      -o "$scratch/win32-smoke.exe" -I/opt/mosh-arm64/include \
      -L/opt/mosh-arm64/lib -lcrypto -lprotobuf -lz -ltinfo -lws2_32 -luser32
    cd /mosh
    make -f win32/Makefile.win clean
    make -j"$(nproc)" -f win32/Makefile.win CXX=aarch64-w64-mingw32-clang++ AR=aarch64-w64-mingw32-ar NM=aarch64-w64-mingw32-nm OBJDUMP=aarch64-w64-mingw32-objdump check' >"$log" 2>&1; then
  status=0
else
  status=$?
fi

grep -E 'error:|fatal error:' "$log" | \
  grep -v '^make' | \
  sort -u || true
exit "$status"
