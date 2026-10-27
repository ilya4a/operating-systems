#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
OUTPUT_PATH="${DISK_MONITOR_OUTPUT:-$SCRIPT_DIR/disk-monitor}"
CXX_COMPILER="${CXX:-g++}"

if ! command -v cmake >/dev/null 2>&1; then
    printf 'ERROR: cmake was not found. Install CMake first.\n' >&2
    exit 1
fi

if ! command -v "$CXX_COMPILER" >/dev/null 2>&1; then
    printf 'ERROR: C++ compiler not found: %s\n' "$CXX_COMPILER" >&2
    exit 1
fi

BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/disk-monitor-build.XXXXXX")"
OUTPUT_DIR="$(dirname -- "$OUTPUT_PATH")"
OUTPUT_NAME="$(basename -- "$OUTPUT_PATH")"
mkdir -p -- "$OUTPUT_DIR"
OUTPUT_TMP="$(mktemp "$OUTPUT_DIR/.${OUTPUT_NAME}.tmp.XXXXXX")"

cleanup() {
    local status=$?
    trap - EXIT
    rm -rf -- "$BUILD_ROOT"
    rm -f -- "$OUTPUT_TMP"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

printf 'Configuring Disk Monitor...\n'
cmake -S "$SCRIPT_DIR" -B "$BUILD_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER"

printf 'Building with -Wall -Wextra -Wpedantic -Werror...\n'
cmake --build "$BUILD_ROOT" --target disk-monitor --parallel

BUILT_BINARY="$BUILD_ROOT/disk-monitor"
if [[ ! -x "$BUILT_BINARY" ]]; then
    printf 'ERROR: CMake completed, but executable was not produced.\n' >&2
    exit 1
fi

install -m 0755 -- "$BUILT_BINARY" "$OUTPUT_TMP"
mv -f -- "$OUTPUT_TMP" "$OUTPUT_PATH"
printf 'Build successful: %s\n' "$OUTPUT_PATH"
printf 'Temporary build files will be removed automatically.\n'