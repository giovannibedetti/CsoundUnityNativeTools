# CsoundUnityNativeTools

Developer-facing build tools for native and JVM artefacts that ship with the
[**CsoundUnity**](https://github.com/rorywalsh/CsoundUnity) Unity package.

The CsoundUnity package itself contains only ready-to-use C# code and
pre-compiled binaries. This repository contains the **source code and build
scripts** used to produce those binaries:

| Folder | What it builds | Where it goes in CsoundUnity |
|---|---|---|
| `native-input/` | macOS bundle, iOS / visionOS static libs, Android `.so`, Windows `.dll` (low-latency multichannel audio input via CoreAudio / AAudio / WASAPI) | `Runtime/macOS/`, `Runtime/iOS/`, `Runtime/VisionOS/`, `Runtime/Android/<abi>/`, `Runtime/Win64/` |
| `android-midi/` | Android MIDI plugin (`CsoundUnityMidi.aar`, wraps `android.media.midi`) | `Runtime/Plugins/Android/` |

## Setup

1. Clone this repo somewhere local.
2. Clone the [CsoundUnity package repo](https://github.com/rorywalsh/CsoundUnity) somewhere local too (typically inside a Unity project's `Packages/CsoundUnity/`).
3. Point this repo at the package via an env var:
   ```bash
   # macOS / Linux
   export CSOUNDUNITY_PACKAGE_PATH=/absolute/path/to/Packages/CsoundUnity
   ```
   ```powershell
   # Windows
   $env:CSOUNDUNITY_PACKAGE_PATH = "C:\absolute\path\to\Packages\CsoundUnity"
   ```

## Building everything for the host

```bash
# macOS / Linux host (builds whatever the host can: macOS, iOS, Android, Android MIDI)
./build-all.sh
```
```powershell
# Windows host (builds Windows DLL)
.\build-all.ps1
```

## Building a single target

See the `README.md` and the per-platform scripts inside each sub-folder.
Each script accepts the same `CSOUNDUNITY_PACKAGE_PATH` env var.

## Workflow

1. Edit source in `native-input/src/` or `android-midi/`.
2. Run the relevant build script (or `build-all`).
3. The resulting binary is copied straight into the CsoundUnity package.
4. Commit the updated binary inside the CsoundUnity package repo (not in
   this one — this repo only tracks the source of those binaries).

## Requirements (summary)

| Build target | Host | Tools |
|---|---|---|
| macOS bundle | macOS | Xcode + CMake 3.18+ |
| iOS / visionOS static | macOS | Xcode (with iOS / visionOS SDK) + CMake |
| Android `.so` (all 4 ABIs) | macOS / Linux | Android NDK + CMake |
| Windows `.dll` | Windows | Visual Studio 2022 (C++ workload) + CMake |
| Android MIDI `.aar` | macOS / Linux | JDK 8+, Android SDK (API 23+), Unity (for `classes.jar`) |
