#!/usr/bin/env bash
# Build CsoundNativeInput.bundle for macOS (universal: arm64 + x86_64) and
# copy it into the CsoundUnity Unity package.
#
# Requirements:
#   - Xcode + Command Line Tools (clang, libc++)
#   - CMake 3.18+
#
# Output: $CSOUNDUNITY_PACKAGE_PATH/Runtime/macOS/CsoundNativeInput.bundle
#
# Usage:
#   export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity
#   ./build-macos.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$CSOUNDUNITY_PACKAGE_PATH" ]; then
    echo "Error: CSOUNDUNITY_PACKAGE_PATH is not set."
    exit 1
fi

BUILD_DIR="$SCRIPT_DIR/build-macos"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DTARGET_MACOS=ON \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCSOUNDUNITY_PACKAGE_PATH="$CSOUNDUNITY_PACKAGE_PATH"

cmake --build . --config Release --parallel

echo "Done: $CSOUNDUNITY_PACKAGE_PATH/Runtime/macOS/CsoundNativeInput.bundle"
