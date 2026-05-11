#!/usr/bin/env bash
# Build libcsnativeinput.so for all four standard Android ABIs and copy each
# into the CsoundUnity Unity package under Runtime/Android/<abi>/.
#
# Requirements:
#   - Android NDK (ANDROID_NDK env var, or "ndkbundle" under ANDROID_HOME)
#   - CMake 3.18+
#
# Output: $CSOUNDUNITY_PACKAGE_PATH/Runtime/Android/<abi>/libcsnativeinput.so
#
# Usage:
#   export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity
#   ./build-android.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$CSOUNDUNITY_PACKAGE_PATH" ]; then
    echo "Error: CSOUNDUNITY_PACKAGE_PATH is not set."
    exit 1
fi

# Locate Android NDK
if [ -z "$ANDROID_NDK" ]; then
    if [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk-bundle" ]; then
        ANDROID_NDK="$ANDROID_HOME/ndk-bundle"
    elif [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk" ]; then
        ANDROID_NDK=$(find "$ANDROID_HOME/ndk" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -1)
    elif [ -d "$HOME/Library/Android/sdk/ndk" ]; then
        ANDROID_NDK=$(find "$HOME/Library/Android/sdk/ndk" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -1)
    fi
fi

if [ -z "$ANDROID_NDK" ] || [ ! -d "$ANDROID_NDK" ]; then
    echo "Error: Android NDK not found. Set ANDROID_NDK env var."
    exit 1
fi
echo "Using Android NDK: $ANDROID_NDK"

ABIS=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")

for ABI in "${ABIS[@]}"; do
    BUILD_DIR="$SCRIPT_DIR/build-android-$ABI"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    echo "─── Building $ABI ───"
    cmake "$SCRIPT_DIR" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-26 \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCSOUNDUNITY_PACKAGE_PATH="$CSOUNDUNITY_PACKAGE_PATH"

    cmake --build . --config Release --parallel
done

echo "Done: $CSOUNDUNITY_PACKAGE_PATH/Runtime/Android/<abi>/libcsnativeinput.so"
