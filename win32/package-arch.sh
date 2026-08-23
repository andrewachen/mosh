# shellcheck shell=bash
# shellcheck disable=SC2154  # arch / llvm_objdump / llvm_readobj are provided by the sourcer
: <<'MOSH_LICENSE'
/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/
MOSH_LICENSE

# ABOUTME: Per-arch PE/COFF identity gate (assert_arch) for the mosh runtime bundle.
# ABOUTME: Sourced by package.sh; also sourced by the docker arch discrimination test.
#
# Sourceable, not executable (no shebang): it requires `arch' (arm64 or x64),
# the LLVM tool paths set by package.sh (llvm_objdump/llvm_readobj), and the
# `die' error helper from package.sh (prints "ERROR: ..." to stderr, exit 1).
# It defines assert_arch FILE LABEL, which returns status 1 unless FILE is a
# linkable PE/COFF image of the requested architecture. Under `set -e'
# (package.sh) a bare call still aborts the script with exit 1, so staging
# fails closed; under the discrimination test the mismatch is a plain non-zero
# status that an `if` can inspect. Sourcing this file runs no staging logic,
# so the arch gate can be unit-tested directly.

# Per-arch PE/COFF identity tokens, verified against LLVM 18 output. Resolved
# per call, not at source time: the helper is sourced by both package.sh (arch
# already set) and the docker discrimination test (arch set per invocation), so
# the case must observe $arch when assert_arch actually runs.
set_arch_tokens() {
  case "$arch" in
    arm64)
      OBJDUMP_FORMAT='file format coff-arm64'
      OBJDUMP_ARCH='architecture: aarch64'
      READOBJ_FORMAT='COFF-ARM64'
      READOBJ_ARCH_RE='Arch:[[:space:]]+aarch64|Machine:[[:space:]]+IMAGE_FILE_MACHINE_ARM64([[:space:]]|$)'
      ;;
    x64)
      OBJDUMP_FORMAT='file format coff-x86-64'
      OBJDUMP_ARCH='architecture: x86_64'
      READOBJ_FORMAT='COFF-x86-64'
      READOBJ_ARCH_RE='Arch:[[:space:]]+x86_64|Machine:[[:space:]]+IMAGE_FILE_MACHINE_AMD64([[:space:]]|$)'
      ;;
    *) die "unknown --arch: $arch (expected arm64 or x64)" ;;
  esac
}

# The LLVM inspection tools are native Windows binaries on MSYS2; hand them a
# Windows-native path (C:\...) rather than an MSYS2 /c/... path, which a native
# tool may fail to open or report against. cygpath is always present on MSYS2.
native_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w -- "$1"
  else
    printf '%s' "$1"
  fi
}

assert_arch() {
  local file=$1
  local label=$2
  local output
  local tool_file
  set_arch_tokens
  tool_file=$(native_path "$file")

  local result=0
  if [[ -n "$llvm_objdump" ]]; then
    output=$("$llvm_objdump" -f "$tool_file" 2>&1) || die "llvm-objdump could not inspect $label: $file"
    # The format line is "<path>: file format coff-arm64"; the path may carry a
    # Windows drive-letter colon (C:\...) once native_path() converts it, so match
    # the format token at end-of-line rather than anchoring on the path prefix.
    if ! grep -Eiq "^[[:space:]]*${OBJDUMP_ARCH}[[:space:]]*$" <<<"$output" ||
       ! grep -Eiq "${OBJDUMP_FORMAT}[[:space:]]*$" <<<"$output"; then
      result=1
    fi
  else
    output=$("$llvm_readobj" --file-headers "$tool_file" 2>&1) || die "llvm-readobj could not inspect $label: $file"
    if ! grep -Eiq "^[[:space:]]*Format:[[:space:]]*${READOBJ_FORMAT}([[:space:]]|$)" <<<"$output" ||
       ! grep -Eiq "^[[:space:]]*(${READOBJ_ARCH_RE})" <<<"$output"; then
      result=1
    fi
  fi
  if ((result)); then
    # Match the plan's mandated wording ("is not an $arch PE/COFF image") and
    # the ERROR: prefix every other package.sh failure emits via die(), while
    # keeping the return-1 behavior the docker discrimination test relies on.
    local tool_name=llvm-readobj
    [[ -n "$llvm_objdump" ]] && tool_name=llvm-objdump
    printf 'ERROR: %s is not an %s PE/COFF image via %s\n%s: %s\n' \
      "$label" "$arch" "$tool_name" "$file" "$output" >&2
    return 1
  fi
  return 0
}
