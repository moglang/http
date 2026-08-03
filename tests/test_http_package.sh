#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: test_http_package.sh /path/to/mog /path/to/http-package" >&2
    exit 2
fi

MOG="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
PACKAGE="$(cd "$2" && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

cmake -S "$PACKAGE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --parallel

STAGE="$BUILD_DIR/stage/github/http"
mkdir -p "$STAGE"
cp -R "$PACKAGE/." "$STAGE/"
if [[ "$(uname -s)" == "Darwin" ]]; then
    PACKAGE_LIBRARY="package.dylib"
else
    PACKAGE_LIBRARY="package.so"
fi
cp "$BUILD_DIR/package.so" "$STAGE/$PACKAGE_LIBRARY"
"$MOG" validate-package "$STAGE"
python3 "$PACKAGE/tests/integration_client.py" "$MOG" "$PACKAGE" "$BUILD_DIR/package.so"

echo "[PASS] HTTP package Milestone 1"
