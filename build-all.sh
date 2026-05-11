#!/usr/bin/env bash
# Master build script (macOS / Linux host).
# Runs every native-build sub-script that is compatible with the current host
# and copies the resulting artefacts into the CsoundUnity Unity package.
#
# Host capabilities:
#   macOS host  → builds macOS, iOS, visionOS, Android, Android MIDI .aar
#   Linux host  → builds Android (Android MIDI .aar if JDK + SDK are available)
#
# For Windows host, see build-all.ps1.
#
# Required env var:
#   CSOUNDUNITY_PACKAGE_PATH  Path to the local CsoundUnity package clone, e.g.
#                              /Users/me/Work/CsoundUnity/Packages/CsoundUnity

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

echo "═══════════════════════════════════════════════════════════════════════"
echo " Target: $CSOUNDUNITY_PACKAGE_PATH"
echo "═══════════════════════════════════════════════════════════════════════"

HOST="$(uname -s)"

run() {
    local label="$1"
    shift
    echo ""
    echo "─── $label ─────────────────────────────────────────────"
    "$@" || echo "  ⚠️  $label failed (skipping)"
}

if [ "$HOST" = "Darwin" ]; then
    run "Native input — macOS"    "$SCRIPT_DIR/native-input/build-macos.sh"
    run "Native input — iOS"      "$SCRIPT_DIR/native-input/build-ios.sh"
    # visionOS optional — uncomment if you have the SDK
    # run "Native input — visionOS" "$SCRIPT_DIR/native-input/build-visionos.sh"
fi

run "Native input — Android"  "$SCRIPT_DIR/native-input/build-android.sh"
run "Android MIDI .aar"       "$SCRIPT_DIR/android-midi/build.sh"

echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo " All compatible builds complete."
echo "═══════════════════════════════════════════════════════════════════════"
