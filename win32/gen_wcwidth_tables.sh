#!/usr/bin/env bash
# ABOUTME: Regenerate and verify the generated astral wcwidth table.
# ABOUTME: Uses the pinned glibc 2.39 C.UTF-8 oracle; run --check in CI.
set -euo pipefail

usage() {
  printf 'Usage: %s [--check]\n' "$0" >&2
  printf 'Generate the glibc 2.39 C.UTF-8 astral wcwidth table.\n' >&2
  printf '  --check  compare generated output with win32/wcwidth.h\n' >&2
}

check=0
case "${1:-}" in
  '') ;;
  --check) check=1 ;;
  --help|-h) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac
if (($# > 1)); then
  printf '%s: too many arguments\n' "$0" >&2
  usage
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

if ! command -v cc >/dev/null 2>&1; then
  printf '%s: cc is required\n' "$0" >&2
  exit 1
fi

cc -std=c11 -Wall -Wextra -Werror \
  "$repo_root/win32/gen_wcwidth_tables.c" -o "$tmp_dir/gen_wcwidth_tables"
"$tmp_dir/gen_wcwidth_tables" > "$tmp_dir/generated.txt"

if ((check)); then
  table=$(mktemp "$tmp_dir/table.XXXXXX")
  awk '/BEGIN GENERATED ASTRAL WIDTH TABLES/{inside=1} inside{print} /END GENERATED ASTRAL WIDTH TABLES/{exit}' \
    "$repo_root/win32/wcwidth.h" > "$table"
  if ! diff -u "$table" "$tmp_dir/generated.txt"; then
    printf '%s: generated table differs from win32/wcwidth.h\n' "$0" >&2
    exit 1
  fi
  printf '%s: checked-in astral width table is current\n' "$0"
else
  cat "$tmp_dir/generated.txt"
fi
