#!/usr/bin/env bash
# Build CsoundUnityMidi.aar from CsoundMidiPlugin.java and copy it into the
# CsoundUnity Unity package (Runtime/Plugins/Android/).
#
# An AAR (Android Archive) is used instead of a plain JAR to avoid the
# "duplicate class" Gradle error (AGP 8+) that occurs when a JAR is resolved
# both as a library runtime artifact and as an external dependency.
#
# Requirements:
#   - JDK 8+ (javac, jar)
#   - Android SDK with at least one platform installed (API 23+)
#     (ANDROID_HOME or default location ~/Library/Android/sdk)
#   - Unity with Android Build Support installed (Unity Hub default location)
#   - zip (pre-installed on macOS/Linux)
#
# Output destination:
#   $CSOUNDUNITY_PACKAGE_PATH/Runtime/Plugins/Android/CsoundUnityMidi.aar
#
# Set CSOUNDUNITY_PACKAGE_PATH to point at your local CsoundUnity package
# clone, e.g.
#   export CSOUNDUNITY_PACKAGE_PATH=~/Work/CsoundUnity/Packages/CsoundUnity
#
# Usage:
#   ./build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$CSOUNDUNITY_PACKAGE_PATH" ]; then
    echo "Error: CSOUNDUNITY_PACKAGE_PATH is not set."
    echo "  export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity"
    exit 1
fi

if [ ! -d "$CSOUNDUNITY_PACKAGE_PATH" ]; then
    echo "Error: CSOUNDUNITY_PACKAGE_PATH does not exist: $CSOUNDUNITY_PACKAGE_PATH"
    exit 1
fi

# ── Locate Android SDK ────────────────────────────────────────────────────────

ANDROID_SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ANDROID_JAR=$(find "$ANDROID_SDK/platforms" -name "android.jar" 2>/dev/null | sort -V | tail -1)

if [ -z "$ANDROID_JAR" ]; then
    echo "Error: android.jar not found. Set ANDROID_HOME or install Android SDK."
    exit 1
fi
echo "Using Android SDK: $ANDROID_JAR"

# ── Locate Unity classes.jar ──────────────────────────────────────────────────

UNITY_HUB_EDITORS="/Applications/Unity/Hub/Editor"
UNITY_CLASSES_JAR=$(find "$UNITY_HUB_EDITORS" -name "classes.jar" -path "*/il2cpp/Release/*" 2>/dev/null | sort -V | tail -1)

if [ -z "$UNITY_CLASSES_JAR" ]; then
    echo "Error: Unity classes.jar not found under $UNITY_HUB_EDITORS"
    exit 1
fi
echo "Using Unity classes.jar: $UNITY_CLASSES_JAR"

# ── Compile ───────────────────────────────────────────────────────────────────

BUILD_DIR="$(mktemp -d)"
trap "rm -rf $BUILD_DIR" EXIT

javac --release 8 \
    -classpath "$ANDROID_JAR:$UNITY_CLASSES_JAR" \
    "$SCRIPT_DIR/CsoundMidiPlugin.java" \
    -d "$BUILD_DIR"

jar cf "$BUILD_DIR/classes.jar" -C "$BUILD_DIR" com/

# ── Package as AAR ────────────────────────────────────────────────────────────
# An AAR is a ZIP containing at minimum AndroidManifest.xml and classes.jar.
# AGP deduplicates AARs correctly; plain JARs can be merged twice (AGP 8+).

cat > "$BUILD_DIR/AndroidManifest.xml" << 'MANIFEST'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.csound.unity" />
MANIFEST

(cd "$BUILD_DIR" && zip -j CsoundUnityMidi.aar AndroidManifest.xml classes.jar)

# ── Copy to package ───────────────────────────────────────────────────────────

DEST="$CSOUNDUNITY_PACKAGE_PATH/Runtime/Plugins/Android"
mkdir -p "$DEST"
# Remove any leftover plain JAR to prevent duplicate class errors
rm -f "$DEST/CsoundUnityMidi.jar"
cp "$BUILD_DIR/CsoundUnityMidi.aar" "$DEST/"

echo "Done: $DEST/CsoundUnityMidi.aar"
