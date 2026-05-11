#!/usr/bin/env bash
# Build libCsoundNativeInput.a for iOS (arm64) and copy it into the CsoundUnity
# Unity package.
#
# Requirements:
#   - Xcode + iOS SDK
#   - CMake 3.18+
#
# Output: $CSOUNDUNITY_PACKAGE_PATH/Runtime/iOS/libCsoundNativeInput.a
#
# Usage:
#   export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity
#   ./build-ios.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$CSOUNDUNITY_PACKAGE_PATH" ]; then
    echo "Error: CSOUNDUNITY_PACKAGE_PATH is not set."
    exit 1
fi

BUILD_DIR="$SCRIPT_DIR/build-ios"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCSOUNDUNITY_PACKAGE_PATH="$CSOUNDUNITY_PACKAGE_PATH"

cmake --build . --config Release

echo "Done: $CSOUNDUNITY_PACKAGE_PATH/Runtime/iOS/libCsoundNativeInput.a"
