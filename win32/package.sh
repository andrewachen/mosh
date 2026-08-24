#!/usr/bin/env bash
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

# ABOUTME: Stages the native Windows mosh runtime (ARM64 or x64), a single static mosh.exe.
# ABOUTME: Verifies imports, architecture, licenses, and a reproducible manifest.
#
# Use this script after building win32/mosh.exe to make the fail-closed
# distribution directory used by packaging and CI. Run `win32/package.sh -h`
# for the command-line interface and defaults.

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: win32/package.sh [--arch {arm64|x64}] [--exe PATH] [--stage DIR]
       [--dll-dirs DIR[:DIR...]] [--zip PATH]

Stage mosh.exe, license notices, and MANIFEST.txt. Every mosh.exe import must
be a Windows system DLL — a non-system import fails the staging, since the
deliverable is a single statically-linked executable. Defaults are arm64
(the default --arch), ./mosh.exe, win32/build/stage/mosh-<arch>, and
${MINGW_PREFIX}/bin respectively (the DLL dirs are used only to locate
license text). --zip also writes a zip archive of the staged directory
(contents at the archive root) for distribution.
EOF
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

arch='arm64'                 # default; --arch overrides
exe='./mosh.exe'
stage=''                     # set after parsing if still empty
dll_dirs=''                  # set after parsing if still empty
zip_path=''

while (($#)); do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --arch)
      (($# >= 2)) || die '--arch requires a value'
      [[ "$2" == arm64 || "$2" == x64 ]] || die '--arch must be arm64 or x64'
      arch=$2
      shift 2
      ;;
    --exe)
      (($# >= 2)) || die '--exe requires a path'
      exe=$2
      shift 2
      ;;
    --stage)
      (($# >= 2)) || die '--stage requires a directory'
      stage=$2
      shift 2
      ;;
    --dll-dirs)
      (($# >= 2)) || die '--dll-dirs requires a colon-separated directory list'
      dll_dirs=$2
      shift 2
      ;;
    --zip)
      (($# >= 2)) || die '--zip requires a path'
      [[ -n "$2" ]] || die '--zip requires a non-empty path'
      zip_path=$2
      shift 2
      ;;
    *)
      die "unknown argument: $1 (use -h for help)"
      ;;
  esac
done

# Defaults that depend on --arch have to wait until after parsing so an
# explicit --stage/--dll-dirs still wins.
[[ -n "$stage" ]] || stage="win32/build/stage/mosh-$arch"
if [[ -z "$dll_dirs" ]]; then
  [[ -n "${MINGW_PREFIX:-}" ]] || die 'MINGW_PREFIX must be set (msys2 sets it); or pass --dll-dirs'
  dll_dirs="$MINGW_PREFIX/bin"
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)

if [[ "$exe" == /* ]]; then
  exe_path=$exe
else
  exe_path="$PWD/$exe"
fi
command -v readlink >/dev/null 2>&1 || die 'readlink is required'
exe_path=$(readlink -f -- "$exe_path") || die "could not resolve executable path: $exe_path"
if [[ "$stage" == /* ]]; then
  stage_dir=$stage
else
  stage_dir="$PWD/$stage"
fi
canonicalize_path() {
  local path=$1
  local parent basename
  if [[ -e "$path" || -L "$path" ]]; then
    readlink -f -- "$path"
    return
  fi
  parent=$(dirname -- "$path")
  basename=$(basename -- "$path")
  parent=$(readlink -f -- "$parent") || die "could not resolve path parent: $path"
  printf '%s/%s\n' "$parent" "$basename"
}

stage_parent=$(dirname -- "$stage_dir")
mkdir -p "$stage_parent"
stage_dir=$(canonicalize_path "$stage_dir") || die "could not resolve stage path: $stage_dir"

[[ -f "$exe_path" ]] || die "executable not found: $exe_path"
exe_parent=$(CDPATH='' cd -- "$(dirname -- "$exe_path")" && pwd)
path_contains() {
  local parent=$1
  local child=$2
  [[ "$parent" == / ]] || parent=${parent%/}
  [[ "$child" == "$parent" || "$child" == "$parent"/* ]]
}
# Reject the filesystem root outright: with stage=/ the containment checks below
# degenerate ("//*" never matches) and rm -rf would be attempted on /, saved only
# by rm's preserve-root. Fail closed instead of depending on the rm implementation.
[[ "$stage_dir" != / ]] || die "refusing to use the filesystem root as the stage path"
[[ "$stage_dir" != "$repo_root" && "$stage_dir" != "$exe_parent" && "$stage_dir" != "$exe_path" ]] || die "refusing to remove stage path: $stage_dir"
path_contains "$stage_dir" "$exe_parent" && die "refusing to remove stage containing executable: $stage_dir"
IFS=: read -r -a safety_dll_dirs <<<"$dll_dirs"
for safety_dll_dir in "${safety_dll_dirs[@]}"; do
  [[ -n "$safety_dll_dir" ]] || continue
  safety_dll_dir=$(canonicalize_path "$safety_dll_dir") || die "could not resolve DLL directory: $safety_dll_dir"
  if path_contains "$stage_dir" "$safety_dll_dir" || path_contains "$safety_dll_dir" "$stage_dir"; then
    die "refusing to remove stage overlapping DLL directory: $stage_dir and $safety_dll_dir"
  fi
done
[[ -r "$exe_path" ]] || die "executable is not readable: $exe_path"
[[ -f "$repo_root/COPYING" ]] || die "missing repository license: $repo_root/COPYING"
[[ -f "$repo_root/debian/copyright" ]] || die "missing copyright file: $repo_root/debian/copyright"

command -v sha256sum >/dev/null 2>&1 || die 'sha256sum is required'
command -v find >/dev/null 2>&1 || die 'find is required'
# Check the zip tool up front like every other tool: failing at the end of the
# run would cost the whole stage, and the rm -f there would have already
# destroyed a previously good archive.
[[ -z "$zip_path" ]] || command -v zip >/dev/null 2>&1 || die 'zip is required for --zip (MSYS2: pacman -S zip)'

llvm_objdump=$(command -v llvm-objdump || true)
llvm_readobj=$(command -v llvm-readobj || true)
[[ -n "$llvm_objdump$llvm_readobj" ]] || die 'no LLVM import inspection tool found (need llvm-objdump or llvm-readobj)'

if [[ -n "$llvm_objdump" ]]; then
  import_tool=$llvm_objdump
  import_mode=llvm-objdump
else
  import_tool=$llvm_readobj
  import_mode=llvm-readobj
fi

version_line() {
  local output version
  output=$("$1" --version 2>&1) || die "could not run $1 --version"
  version=$(grep -m1 -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' <<<"$output" || true)
  [[ -n "$version" ]] || die "could not parse version from $1 --version"
  printf '%s' "$version"
}

import_tool_version=$(version_line "$import_tool")
llvm_objdump_version='unavailable'
llvm_readobj_version='unavailable'
[[ -n "$llvm_objdump" ]] && llvm_objdump_version=$(version_line "$llvm_objdump")
[[ -n "$llvm_readobj" ]] && llvm_readobj_version=$(version_line "$llvm_readobj")
objdump_version='not used (GNU architecture assertions are unsupported)'

# The per-arch PE/COFF identity gate (assert_arch) and its helper native_path
# live in package-arch.sh so the docker discrimination test can source exactly
# that gate — not the whole staging body — and exercise match/mismatch for both
# architectures. native_path converts a path to Windows-native form (C:\...)
# for the LLVM tools, which are native Windows binaries on MSYS2.
# shellcheck source=package-arch.sh
. "$script_dir/package-arch.sh"

dump_imports() {
  local file=$1
  local output line name
  local tool_file
  tool_file=$(native_path "$file")
  if [[ "$import_mode" == llvm-readobj ]]; then
    output=$("$import_tool" --coff-imports "$tool_file" 2>&1) || die "$import_mode could not inspect imports for $file"
  else
    output=$("$import_tool" -p "$tool_file" 2>&1) || die "$import_mode could not inspect imports for $file"
  fi
  IMPORT_NAMES=()
  while IFS= read -r line; do
    line=${line//$'\r'/}
    if [[ "$line" == *'DLL Name: '* ]]; then
      name=${line#*DLL Name: }
      [[ -n "$name" ]] && IMPORT_NAMES+=("$name")
    elif [[ "$import_mode" == llvm-readobj && "$line" =~ ^[[:space:]]*Name:[[:space:]]+([^[:space:]]+\.[Dd][Ll][Ll])[[:space:]]*$ ]]; then
      IMPORT_NAMES+=("${BASH_REMATCH[1]}")
    fi
  done <<<"$output"
}

classify_import() {
  local lower=$1
  case "$lower" in
    msvcrt.dll)
      IMPORT_CLASS=msvcrt
      ;;
    # Exact API-set contracts observed for this build, not an api-ms-win-*
    # wildcard: an invented API-set-shaped name must fail closed like any
    # other non-system import. A future Windows SDK that adds a contract
    # fails here until the name is reviewed and added.
    api-ms-win-crt-stdio-l1-1-0.dll|api-ms-win-crt-runtime-l1-1-0.dll|api-ms-win-crt-locale-l1-1-0.dll|api-ms-win-crt-heap-l1-1-0.dll|api-ms-win-crt-private-l1-1-0.dll|api-ms-win-crt-string-l1-1-0.dll|api-ms-win-crt-convert-l1-1-0.dll|api-ms-win-crt-environment-l1-1-0.dll|api-ms-win-crt-math-l1-1-0.dll|api-ms-win-crt-time-l1-1-0.dll|api-ms-win-crt-multibyte-l1-1-0.dll|api-ms-win-crt-filesystem-l1-1-0.dll|api-ms-win-crt-utility-l1-1-0.dll|ucrtbase.dll|kernel32.dll|advapi32.dll|user32.dll|gdi32.dll|shell32.dll|ole32.dll|oleaut32.dll|ws2_32.dll|crypt32.dll|bcrypt.dll|dbghelp.dll|ntdll.dll|sechost.dll|rpcrt4.dll|shlwapi.dll|version.dll|imm32.dll|setupapi.dll|winmm.dll|wldap32.dll|normaliz.dll|comctl32.dll|comdlg32.dll|msimg32.dll)
      IMPORT_CLASS=system
      ;;
    msys-2.0*.dll|cygwin*.dll|*libstdc++*.dll|*libc++*.dll|*libgcc*.dll|*libunwind*.dll|*libwinpthread*.dll|*libssp*.dll|*libatomic*.dll|*protobuf*.dll|*abseil*.dll|*absl*.dll|*utf8*.dll|msvcp*.dll|vcruntime*.dll)
      IMPORT_CLASS=forbidden
      ;;
    # The deliverable is a single statically-linked mosh.exe, so every
    # non-system import is unexpected. Fail closed rather than silently
    # bundling a surprise DLL.
    *)
      IMPORT_CLASS=unexpected
      ;;
  esac
}

# Start from a clean output directory so stale files cannot escape into the bundle.
rm -rf "$stage_dir"
mkdir -p "$stage_dir" "$stage_dir/licenses"

declare -A staged_sources=()
IMPORT_RECORDS=()

copy_staged() {
  local source=$1
  local destination=$2
  mkdir -p "$(dirname -- "$destination")"
  cp "$source" "$destination"
  staged_sources["$destination"]=$source
}

copy_staged "$exe_path" "$stage_dir/mosh.exe"
assert_arch "$exe_path" 'mosh.exe'

# Every mosh.exe import must classify as a Windows system DLL; anything else is
# rejected inside the loop. There is no transitive closure to walk: the exe is
# the only module in the bundle.
dump_imports "$exe_path"
if ((${#IMPORT_NAMES[@]} == 0)); then
  die 'could not parse any imports from root executable mosh.exe'
fi
for import_name in "${IMPORT_NAMES[@]}"; do
  import_key=${import_name,,}
  classify_import "$import_key"
  if [[ "$IMPORT_CLASS" == msvcrt ]]; then
    die 'msvcrt.dll import indicates a non-UCRT build; the deliverable target is UCRT'
  fi
  if [[ "$IMPORT_CLASS" == system ]]; then
    IMPORT_RECORDS+=("SYSTEM $import_name")
    continue
  fi
  if [[ "$IMPORT_CLASS" == forbidden ]]; then
    die "forbidden runtime import $import_name in mosh.exe"
  fi
  if [[ "$IMPORT_CLASS" == unexpected ]]; then
    die "unexpected non-system import $import_name in mosh.exe (the deliverable is a single statically-linked mosh.exe; a dynamic dependency means the static fold regressed)"
  fi
done

copy_license_tree() {
  local source_root=$1
  local destination_root=$2
  local source_file relative destination find_tmp
  local -a files=()
  find_tmp=$(mktemp "${TMPDIR:-/tmp}/mosh-package-find.XXXXXX")
  if ! find "$source_root" -type f -print0 >"$find_tmp"; then
    rm -f "$find_tmp"
    die "could not traverse license tree: $source_root"
  fi
  mapfile -d '' -t files <"$find_tmp"
  rm -f "$find_tmp"
  LICENSE_COPY_COUNT=${#files[@]}
  ((LICENSE_COPY_COUNT > 0)) || return 0
  for source_file in "${files[@]}"; do
    relative=${source_file#"$source_root"/}
    destination="$destination_root/$relative"
    copy_staged "$source_file" "$destination"
  done
}

copy_pacman_licenses() {
  local package=$1
  local destination_root=$2
  local listing license_path relative destination
  local -a license_paths=()
  listing=$(pacman -Ql "$package" 2>&1) || die "pacman could not list package $package: $listing"
  while IFS= read -r license_path; do
    [[ -n "$license_path" ]] || continue
    [[ -f "$license_path" ]] || continue
    if [[ "$license_path" == */share/licenses/* ]]; then
      relative=${license_path##*/share/licenses/}
    elif [[ "$license_path" == */share/doc/* ]]; then
      relative=${license_path##*/share/doc/}
    else
      continue
    fi
    license_paths+=("$license_path")
    destination="$destination_root/$relative"
    copy_staged "$license_path" "$destination"
  done < <(awk '{for (i = 2; i <= NF; ++i) if ($i ~ /^\/.*\/share\/(licenses|doc)\//) {print $i; break}}' <<<"$listing")
  ((${#license_paths[@]} > 0)) || die "no license text found for package $package"
}

stage_static_license() {
  local component=$1
  local destination_root=$2
  local license_root license_directory owner package pattern destination_name
  local license_root_index=0
  local -a license_names=()
  local -a package_patterns=()
  local found_license=0
  declare -A copied_license_directories=()

  case "$component" in
    protobuf)
      license_names=('protobuf' '*protobuf*')
      package_patterns=('*protobuf*')
      ;;
    abseil)
      license_names=('abseil' 'abseil-cpp' 'absl' '*abseil*' '*absl*' '*utf8*')
      package_patterns=('*abseil*' '*absl*' '*protobuf*')
      ;;
    llvm-c++-runtime)
      license_names=('llvm' 'libc++' 'libc++abi' 'libunwind' 'compiler-rt' '*llvm*' '*libc++*' '*libc++abi*' '*libunwind*' '*compiler-rt*' '*clang*')
      package_patterns=('*llvm*' '*clang*' '*libc++*' '*libunwind*')
      ;;
    winpthreads)
      license_names=('*winpthread*' '*winpthreads*')
      package_patterns=('*winpthread*' '*winpthreads*')
      ;;
    openssl)
      license_names=('openssl' '*openssl*')
      package_patterns=('*openssl*')
      ;;
    ncurses)
      license_names=('ncurses' '*ncurses*')
      package_patterns=('*ncurses*')
      ;;
    zlib)
      license_names=('zlib' '*zlib*')
      package_patterns=('*zlib*')
      ;;
    *)
      die "unknown static license component: $component"
      ;;
  esac

  for license_root in "${license_roots[@]}"; do
    [[ -d "$license_root" ]] || continue
    license_root_index=$((license_root_index + 1))
    while IFS= read -r license_directory; do
      destination_name=$(basename -- "$license_directory")
      if [[ -n "${copied_license_directories[$destination_name]+copied}" &&
            "${copied_license_directories[$destination_name]}" != "$license_directory" ]]; then
        destination_name="$destination_name-root$license_root_index"
      fi
      copied_license_directories["$destination_name"]=$license_directory
      copy_license_tree "$license_directory" "$destination_root/$destination_name"
      if ((LICENSE_COPY_COUNT > 0)); then
        found_license=1
      fi
    done < <(
      for license_name in "${license_names[@]}"; do
        find "$license_root" -mindepth 1 -maxdepth 1 -type d -iname "$license_name" -print
      done | sort -u
    )
  done
  ((found_license)) && return 0

  command -v pacman >/dev/null 2>&1 || die "license text not found for statically folded $component and pacman is unavailable"
  package_patterns=()
  case "$component" in
    protobuf) package_patterns=('*protobuf*') ;;
    abseil) package_patterns=('*abseil*' '*absl*' '*protobuf*') ;;
    llvm-c++-runtime) package_patterns=('*llvm*' '*clang*' '*libc++*' '*libunwind*') ;;
    winpthreads) package_patterns=('*winpthread*' '*winpthreads*') ;;
    openssl) package_patterns=('*openssl*') ;;
    ncurses) package_patterns=('*ncurses*') ;;
    zlib) package_patterns=('*zlib*') ;;
  esac
  while IFS= read -r package; do
    for pattern in "${package_patterns[@]}"; do
      # shellcheck disable=SC2254
      case "$package" in
        $pattern)
          copy_pacman_licenses "$package" "$destination_root/$package"
          found_license=1
          break
          ;;
      esac
    done
  done < <(pacman -Qq 2>/dev/null | sort)
  ((found_license)) || die "license text not found for statically folded $component"
}

# Static-license roots, deduplicated so no root is staged twice. The MSYS2
# license root comes first when MINGW_PREFIX is set (it is the canonical source
# for the default dll_dirs), then the /opt/mosh-$arch cross-image dep tree. The
# per-dll-dir roots below cover an explicit --dll-dirs override (which is how a
# caller outside MSYS2 supplies license text), so a missing MINGW_PREFIX here
# must NOT abort: only add the MSYS2 root when MINGW_PREFIX is actually set.
# A root already seeded by an earlier step is skipped (the default dll_dirs
# derives from MINGW_PREFIX and would otherwise repeat the MSYS2 root).
# (--help exits before licensing starts, so it is never affected by a missing
# MINGW_PREFIX.)
license_roots=()
[[ -n "${MINGW_PREFIX:-}" ]] && license_roots+=("$MINGW_PREFIX/share/licenses")
license_roots+=("/opt/mosh-$arch/share/licenses")
IFS=: read -r -a DLL_DIR_LIST <<<"$dll_dirs"
for dll_dir in "${DLL_DIR_LIST[@]}"; do
  [[ -n "$dll_dir" ]] || continue
  root="$(dirname -- "$dll_dir")/share/licenses"
  # Skip a root already seeded above (default dll_dirs derives from MINGW_PREFIX).
  [[ " ${license_roots[*]} " == *" $root "* ]] || license_roots+=("$root")
done

static_license_destination="$stage_dir/licenses/static"
stage_static_license protobuf "$static_license_destination/protobuf"
stage_static_license abseil "$static_license_destination/abseil"
stage_static_license llvm-c++-runtime "$static_license_destination/llvm-c++-runtime"
stage_static_license winpthreads "$static_license_destination/winpthreads"
stage_static_license openssl "$static_license_destination/openssl"
stage_static_license ncurses "$static_license_destination/ncurses"
stage_static_license zlib "$static_license_destination/zlib"

ocb_notice="$static_license_destination/ocb/ISC-NOTICE"
mkdir -p "$(dirname -- "$ocb_notice")"
# Extract the ISC license header (Krovetz copyright line through the ISC
# footer). The footer text "USE OR PERFORMANCE OF THIS SOFTWARE." is a suffix
# of its source line, not the whole line, so match it as a substring.
awk '
  /^\/ Copyright \(c\) 2012 Ted Krovetz\.$/ { in_notice = 1 }
  in_notice {
    sub(/^\/ ?/, "")
    print
    if (/USE OR PERFORMANCE OF THIS SOFTWARE\./) exit
  }
' "$repo_root/src/crypto/ocb_internal.cc" >"$ocb_notice"
[[ -s "$ocb_notice" ]] || die 'could not extract the OCB ISC notice from src/crypto/ocb_internal.cc'
staged_sources["$ocb_notice"]="$repo_root/src/crypto/ocb_internal.cc"

copy_staged "$repo_root/COPYING" "$stage_dir/licenses/COPYING.mosh"
openssl_exception="$stage_dir/licenses/OPENSSL-EXCEPTION.mosh"
awk '
  /^ In addition, as a special exception, the copyright holders give$/ { in_exception = 1 }
  in_exception && /^ On Debian systems,/ { exit }
  in_exception {
    if ($0 == " .") {
      print ""
    } else {
      sub(/^ /, "")
      print
    }
  }
' "$repo_root/debian/copyright" >"$openssl_exception"
[[ -s "$openssl_exception" ]] || die 'could not extract the OpenSSL exception from debian/copyright'
staged_sources["$openssl_exception"]="$repo_root/debian/copyright"

manifest="$stage_dir/MANIFEST.txt"
manifest_tmp=$(mktemp "${TMPDIR:-/tmp}/mosh-package-manifest.XXXXXX")
trap 'rm -f "$manifest_tmp"' EXIT
{
  printf 'mosh %s runtime bundle\n' "$arch"
  printf 'Executable source: %s\n' "$exe_path"
  printf 'DLL search dirs: %s\n' "$dll_dirs"
  printf 'Static components folded into mosh.exe: protobuf, abseil, LLVM C++ runtime (libc++/libunwind/compiler-rt), OCB, winpthreads, OpenSSL (libcrypto), zlib, ncurses\n'
  printf '\nStaged payload files (MANIFEST.txt is self-excluded):\n'
  while IFS= read -r staged_file; do
    hash=$(sha256sum "$staged_file")
    hash=${hash%% *}
    size=$(stat -c '%s' "$staged_file")
    source=${staged_sources[$staged_file]:-unknown}
    printf '%s %s %s source=%s\n' "$hash" "$size" "${staged_file#"$stage_dir"/}" "$source"
  done < <(find "$stage_dir" -type f -print | sort)
  printf '\nClassified imports:\n'
  for import_record in "${IMPORT_RECORDS[@]}"; do
    printf '%s\n' "$import_record"
  done
  printf '\nInspection tools:\n'
  printf 'import-dump: %s\n' "$import_mode"
  printf 'import-tool-version: %s\n' "$import_tool_version"
  printf 'llvm-objdump-version: %s\n' "$llvm_objdump_version"
  printf 'llvm-readobj-version: %s\n' "$llvm_readobj_version"
  printf 'objdump-version: %s\n' "$objdump_version"
  printf 'architecture-assertion: LLVM\n'
} >"$manifest_tmp"
mv "$manifest_tmp" "$manifest"

printf 'Staged %s runtime bundle in %s\n' "$arch" "$stage_dir"

if [[ -n "$zip_path" ]]; then
  if [[ "$zip_path" != /* ]]; then
    zip_path="$PWD/$zip_path"
  fi
  # Create the parent tree before canonicalizing: canonicalize_path resolves the
  # parent with readlink -f, which fails on a multi-level missing destination.
  mkdir -p "$(dirname -- "$zip_path")"
  zip_path=$(canonicalize_path "$zip_path") || die "could not resolve zip path: $zip_path"
  path_contains "$stage_dir" "$zip_path" && die "refusing to write the zip inside the stage directory: $zip_path"
  [[ "$zip_path" != "$exe_path" ]] || die "refusing to overwrite the executable with the zip: $zip_path"
  [[ ! -d "$zip_path" ]] || die "zip path is a directory: $zip_path"
  rm -f -- "$zip_path"
  # Archive the stage contents at the zip root (mosh.exe at top level, not under
  # a mosh-<arch>/ directory) so the archive can be unzipped and run in place.
  (cd "$stage_dir" && zip -q -r -X "$zip_path" .) || die "zip failed: $zip_path"
  [[ -s "$zip_path" ]] || die "zip did not produce a non-empty archive: $zip_path"
  printf 'Wrote zip archive %s\n' "$zip_path"
fi
