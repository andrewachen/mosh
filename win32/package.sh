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

# ABOUTME: Stages the native Windows ARM64 mosh runtime and its DLL closure.
# ABOUTME: Verifies imports, architecture, licenses, and a reproducible manifest.
#
# Use this script after building win32/mosh.exe to make the fail-closed
# distribution directory used by packaging and CI. Run `win32/package.sh -h`
# for the command-line interface and defaults.

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: win32/package.sh [--exe PATH] [--stage DIR] [--dll-dirs DIR[:DIR...]] [--zip PATH]

Stage mosh.exe, its transitive non-system DLL dependencies, license notices,
and MANIFEST.txt. Defaults are ./mosh.exe, win32/build/stage/mosh-arm64, and
${MINGW_PREFIX:-/clangarm64}/bin respectively. --zip also writes a zip archive
of the staged directory (contents at the archive root) for distribution.
EOF
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

warn() {
  printf 'WARNING: %s\n' "$*" >&2
}

exe='./mosh.exe'
stage='win32/build/stage/mosh-arm64'
dll_dirs="${MINGW_PREFIX:-/clangarm64}/bin"
zip_path=''

while (($#)); do
  case "$1" in
    -h|--help)
      usage
      exit 0
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
      zip_path=$2
      shift 2
      ;;
    *)
      die "unknown argument: $1 (use -h for help)"
      ;;
  esac
done

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

assert_arm64() {
  local file=$1
  local label=$2
  local output
  local tool_file
  tool_file=$(native_path "$file")

  if [[ -n "$llvm_objdump" ]]; then
    output=$("$llvm_objdump" -f "$tool_file" 2>&1) || die "llvm-objdump could not inspect $label: $file"
    # The format line is "<path>: file format coff-arm64"; the path may carry a
    # Windows drive-letter colon (C:\...) once native_path() converts it, so match
    # the format token at end-of-line rather than anchoring on the path prefix.
    if ! grep -Eiq '^[[:space:]]*architecture: aarch64[[:space:]]*$' <<<"$output" ||
       ! grep -Eiq 'file format coff-arm64[[:space:]]*$' <<<"$output"; then
      printf 'llvm-objdump -f output for %s follows (arch assertion failed):\n%s\n' "$file" "$output" >&2
      die "$label is not an ARM64 PE/COFF image according to llvm-objdump: $file"
    fi
  else
    output=$("$llvm_readobj" --file-headers "$tool_file" 2>&1) || die "llvm-readobj could not inspect $label: $file"
    if ! grep -Eiq '^[[:space:]]*Format:[[:space:]]*COFF-ARM64([[:space:]]|$)' <<<"$output" ||
       ! grep -Eiq '^[[:space:]]*(Arch:[[:space:]]+aarch64|Machine:[[:space:]]+IMAGE_FILE_MACHINE_ARM64([[:space:]]|$))' <<<"$output"; then
      printf 'llvm-readobj --file-headers output for %s follows (arch assertion failed):\n%s\n' "$file" "$output" >&2
      die "$label is not an ARM64 PE/COFF image according to llvm-readobj: $file"
    fi
  fi
}

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
    api-ms-win-*|ucrtbase.dll|kernel32.dll|advapi32.dll|user32.dll|gdi32.dll|shell32.dll|ole32.dll|oleaut32.dll|ws2_32.dll|crypt32.dll|bcrypt.dll|dbghelp.dll|ntdll.dll|sechost.dll|rpcrt4.dll|shlwapi.dll|version.dll|imm32.dll|setupapi.dll|winmm.dll|wldap32.dll|normaliz.dll|comctl32.dll|comdlg32.dll|msimg32.dll)
      IMPORT_CLASS=system
      ;;
    msys-2.0*.dll|cygwin*.dll|*libstdc++*.dll|*libc++*.dll|*libgcc*.dll|*libunwind*.dll|*libwinpthread*.dll|*libssp*.dll|*libatomic*.dll|*protobuf*.dll|*abseil*.dll|*absl*.dll|*utf8*.dll|msvcp*.dll|vcruntime*.dll)
      IMPORT_CLASS=forbidden
      ;;
    *)
      IMPORT_CLASS=shipped
      ;;
  esac
}

find_dll() {
  local name=$1
  local dir candidate
  DLL_PATH=''
  IFS=: read -r -a DLL_DIR_LIST <<<"$dll_dirs"
  for dir in "${DLL_DIR_LIST[@]}"; do
    [[ -n "$dir" ]] || continue
    if [[ -d "$dir" ]]; then
      candidate=$(find "$dir" -maxdepth 1 -type f -iname "$name" -print -quit)
      if [[ -n "$candidate" ]]; then
        DLL_PATH=$candidate
        return 0
      fi
    fi
  done
  return 0
}

# Start from a clean output directory so stale DLLs cannot escape the closure.
rm -rf "$stage_dir"
mkdir -p "$stage_dir" "$stage_dir/licenses"

declare -A staged_sources=()
declare -A dll_sources=()
declare -A seen_modules=()
declare -A seen_imports=()
STAGED_DLL_KEYS=()
IMPORT_RECORDS=()

copy_staged() {
  local source=$1
  local destination=$2
  mkdir -p "$(dirname -- "$destination")"
  cp "$source" "$destination"
  staged_sources["$destination"]=$source
}

copy_staged "$exe_path" "$stage_dir/mosh.exe"
assert_arm64 "$exe_path" 'mosh.exe'

queue_paths=("$exe_path")
queue_labels=('mosh.exe')
queue_index=0
visit_count=0
max_visits=256

while ((queue_index < ${#queue_paths[@]})); do
  visit_count=$((visit_count + 1))
  ((visit_count <= max_visits)) || die "import closure exceeded visit cap of $max_visits"
  module_path=${queue_paths[$queue_index]}
  module_label=${queue_labels[$queue_index]}
  queue_index=$((queue_index + 1))

  dump_imports "$module_path"
  if [[ "$module_label" == mosh.exe && ${#IMPORT_NAMES[@]} -eq 0 ]]; then
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
      die "forbidden runtime import $import_name in $module_label"
    fi

    if [[ -n "${seen_imports[$import_key]+seen}" ]]; then
      IMPORT_RECORDS+=("SHIPPED $import_name -> ${dll_sources[$import_key]}")
      continue
    fi
    seen_imports["$import_key"]=1

    find_dll "$import_name"
    [[ -n "$DLL_PATH" ]] || die "DLL not found: $import_name (searched: $dll_dirs)"
    assert_arm64 "$DLL_PATH" "shipped DLL $import_name"
    dll_sources["$import_key"]=$DLL_PATH
    STAGED_DLL_KEYS+=("$import_key")
    destination="$stage_dir/$(basename -- "$DLL_PATH")"
    if [[ -z "${staged_sources[$destination]+staged}" ]]; then
      copy_staged "$DLL_PATH" "$destination"
    fi
    IMPORT_RECORDS+=("SHIPPED $import_name -> $DLL_PATH")

    if [[ -z "${seen_modules[$import_key]+seen}" ]]; then
      seen_modules["$import_key"]=1
      queue_paths+=("$DLL_PATH")
      queue_labels+=("$import_name")
    fi
  done
done

if ((${#STAGED_DLL_KEYS[@]} == 0)); then
  printf 'NOTE: mosh.exe has an empty non-system DLL closure; staging the executable and notices only.\n'
fi

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

mingw_prefix="${MINGW_PREFIX:-/clangarm64}"
license_roots=("$mingw_prefix/share/licenses" "/opt/mosh-arm64/share/licenses")
IFS=: read -r -a DLL_DIR_LIST <<<"$dll_dirs"
for dll_dir in "${DLL_DIR_LIST[@]}"; do
  [[ -n "$dll_dir" ]] || continue
  license_roots+=("$(dirname -- "$dll_dir")/share/licenses")
done

declare -A installed_license_keys=()
for dll_key in "${STAGED_DLL_KEYS[@]}"; do
  [[ -n "${installed_license_keys[$dll_key]+installed}" ]] && continue
  installed_license_keys[$dll_key]=1
  dll_path=${dll_sources[$dll_key]}
  case "$dll_key" in
    libcrypto*.dll|libssl*.dll) license_key=openssl ;;
    libncurses*.dll|libtinfo*.dll) license_key=ncurses ;;
    zlib1.dll|libz*.dll) license_key=zlib ;;
    *) license_key='' ;;
  esac

  license_destination="$stage_dir/licenses/${license_key:-$dll_key}"
  license_count=0
  if [[ -n "$license_key" ]]; then
    dll_path_resolved=$(readlink -f -- "$dll_path") || die "could not resolve shipped DLL path: $dll_path"
    dll_license_root="$(dirname -- "$(dirname -- "$dll_path_resolved")")/share/licenses"
    dll_license_roots=("$dll_license_root")
    for license_root in "${license_roots[@]}"; do
      [[ "$license_root" == "$dll_license_root" ]] || dll_license_roots+=("$license_root")
    done
    for license_root in "${dll_license_roots[@]}"; do
      license_directory="$license_root/$license_key"
      [[ -d "$license_directory" ]] || continue
      copy_license_tree "$license_directory" "$license_destination"
      license_count=$LICENSE_COPY_COUNT
      ((license_count > 0)) && break
    done
  fi

  if ((license_count == 0)); then
    command -v pacman >/dev/null 2>&1 || die "license text not found for shipped DLL $dll_path and pacman is unavailable"
    owner=$(pacman -Qo "$dll_path" 2>/dev/null || true)
    package=$(awk '{for (i = 1; i <= NF; ++i) if ($i == "by") {print $(i + 1); exit}}' <<<"$owner")
    [[ -n "$package" ]] || die "could not determine owning package for shipped DLL $dll_path"
    copy_pacman_licenses "$package" "$license_destination"
  fi
done

static_license_destination="$stage_dir/licenses/static"
stage_static_license protobuf "$static_license_destination/protobuf"
stage_static_license abseil "$static_license_destination/abseil"
stage_static_license llvm-c++-runtime "$static_license_destination/llvm-c++-runtime"
stage_static_license winpthreads "$static_license_destination/winpthreads"

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
  printf 'mosh ARM64 runtime bundle\n'
  printf 'Executable source: %s\n' "$exe_path"
  printf 'DLL search dirs: %s\n' "$dll_dirs"
  printf 'Static components folded into mosh.exe: protobuf, abseil, LLVM C++ runtime (libc++/libunwind/compiler-rt), OCB, winpthreads\n'
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

printf 'Staged ARM64 runtime bundle in %s\n' "$stage_dir"

if [[ -n "$zip_path" ]]; then
  if [[ "$zip_path" != /* ]]; then
    zip_path="$PWD/$zip_path"
  fi
  zip_path=$(canonicalize_path "$zip_path") || die "could not resolve zip path: $zip_path"
  path_contains "$stage_dir" "$zip_path" && die "refusing to write the zip inside the stage directory: $zip_path"
  mkdir -p "$(dirname -- "$zip_path")"
  rm -f -- "$zip_path"
  # Archive the stage contents at the zip root (mosh.exe at top level, not under
  # a mosh-arm64/ directory) so the archive can be unzipped and run in place.
  command -v zip >/dev/null 2>&1 || die 'zip is required for --zip (MSYS2: pacman -S zip)'
  (cd "$stage_dir" && zip -q -r -X "$zip_path" .) || die "zip failed: $zip_path"
  [[ -s "$zip_path" ]] || die "zip did not produce a non-empty archive: $zip_path"
  printf 'Wrote zip archive %s\n' "$zip_path"
fi
