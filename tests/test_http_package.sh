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

SOURCE_STAGE="$BUILD_DIR/source"
mkdir -p "$SOURCE_STAGE"
cp -R "$PACKAGE/." "$SOURCE_STAGE/"
if [[ -d "$SOURCE_STAGE/.git" ]]; then
    rm -r "$SOURCE_STAGE/.git"
fi

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Debug)
if [[ "${MOG_HTTP_SANITIZER:-}" == "asan-ubsan" ]]; then
    CMAKE_ARGS+=(-DMOG_HTTP_ENABLE_ASAN_UBSAN=ON)
elif [[ "${MOG_HTTP_SANITIZER:-}" == "tsan" ]]; then
    CMAKE_ARGS+=(-DMOG_HTTP_ENABLE_TSAN=ON)
fi
cmake -S "$SOURCE_STAGE" -B "$BUILD_DIR/build" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR/build" --parallel

STAGE="$BUILD_DIR/stage/github/http"
mkdir -p "$STAGE"
cp -R "$SOURCE_STAGE/." "$STAGE/"
if [[ "$(uname -s)" == "Darwin" ]]; then
    PACKAGE_LIBRARY="package.dylib"
else
    PACKAGE_LIBRARY="package.so"
fi
cp "$BUILD_DIR/build/package.so" "$STAGE/$PACKAGE_LIBRARY"
"$MOG" validate-package "$STAGE"
python3 "$PACKAGE/tests/integration_client.py" "$MOG" "$SOURCE_STAGE" "$BUILD_DIR/build/package.so"

echo "[PASS] HTTP/WebSocket package v1"
