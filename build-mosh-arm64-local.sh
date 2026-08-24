#!/bin/bash
# ABOUTME: Back-compat shim — ARM64 local cross-build via build-mosh-local.sh.
# ABOUTME: Kept so existing muscle memory and docs keep working.
set -e
exec env MOSH_ARCH=arm64 "$(dirname -- "$0")/build-mosh-local.sh" "$@"
