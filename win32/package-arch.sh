# ABOUTME: Per-arch PE/COFF identity gate, shared by win32/package.sh and
# win32/package-arch.sh's docker discrimination test.
#
# Sourceable, not executable (no shebang): it requires `arch' (arm64 or x64)
# and the LLVM tool paths set by package.sh (llvm_objdump/llvm_readobj). It
# defines assert_arch FILE LABEL, which returns status 1 unless FILE is a
# linkable PE/COFF image of the requested architecture. Under `set -e`
# (package.sh) a bare call still aborts the script with exit 1, so staging
# fails closed; under the discrimination test the mismatch is a plain non-zero
# status that an `if` can inspect. Sourcing this file runs no staging logic,
# so the arch gate can be unit-tested directly.
# shellcheck shell=bash
# shellcheck disable=SC2154  # arch / llvm_readobj are provided by the sourcer

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
    printf 'architecture assertion failed for %s:\n%s\n' "$label" "$output" >&2
    return 1
  fi
  return 0
}
