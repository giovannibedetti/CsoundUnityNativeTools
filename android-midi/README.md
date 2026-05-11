# Android MIDI Plugin

Java source for `CsoundUnityMidi.aar`, the Android plugin that exposes
`android.media.midi.MidiManager` to CsoundUnity. Supports USB + BLE MIDI
on Android 6.0+ (API 23+).

The compiled `.aar` lives in the CsoundUnity package under
`Runtime/Plugins/Android/CsoundUnityMidi.aar`.

## Build

Set the env var, then run:
```bash
export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity
./build.sh
```

The script:
1. Locates Android SDK (`ANDROID_HOME` or `~/Library/Android/sdk`).
2. Locates a Unity `classes.jar` under `/Applications/Unity/Hub/Editor`
   (Android Build Support must be installed).
3. Compiles `CsoundMidiPlugin.java` with `javac --release 8`.
4. Packages `classes.jar` + a minimal `AndroidManifest.xml` into
   `CsoundUnityMidi.aar` (AAR — not JAR — to avoid the AGP 8+
   "duplicate class" error).
5. Copies the `.aar` into `$CSOUNDUNITY_PACKAGE_PATH/Runtime/Plugins/Android/`.

## Requirements

| Tool | Notes |
|---|---|
| JDK 8+ | Provides `javac` and `jar` |
| Android SDK | API 23+ (`android.jar` is found automatically) |
| Unity with Android Build Support | Needed for `classes.jar` (`UnityPlayer` API) |
| `zip` | Pre-installed on macOS / Linux |
