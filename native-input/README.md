# Native Input

Low-latency multichannel audio input for CsoundUnity, exposed to Unity as a
native plugin per platform.

| Platform | Backend | Output artefact |
|---|---|---|
| macOS | CoreAudio (AUHAL) | `CsoundNativeInput.bundle` |
| iOS / visionOS | AudioUnit RemoteIO + AVAudioSession | `libCsoundNativeInput.a` |
| Android | AAudio (Exclusive / MMAP) | `libcsnativeinput.so` per ABI |
| Windows | WASAPI | `CsoundNativeInput.dll` |

## Source layout

```
src/
├── cni_apple.mm        macOS + iOS + visionOS (CoreAudio / AudioUnit)
├── cni_coreaudio.mm    early macOS-only implementation, kept for reference
├── cni_aaudio.cpp      Android (AAudio)
├── cni_wasapi.cpp      Windows (WASAPI)
└── cni_ringbuffer.h    shared lock-free ring buffer
```

## Build

Set the env var:
```bash
export CSOUNDUNITY_PACKAGE_PATH=/path/to/Packages/CsoundUnity
```

Then run the relevant script:

| Host | Script |
|---|---|
| macOS | `./build-macos.sh`  |
| macOS (iOS) | `./build-ios.sh`    |
| macOS / Linux | `./build-android.sh` (all 4 ABIs) |
| Windows (PowerShell) | `.\build-windows.ps1` |

Each script invokes CMake, builds, and copies the artefact into the
CsoundUnity package via the `CSOUNDUNITY_PACKAGE_PATH` env var.

## Notes

- Android: set Unity → Project Settings → Audio → System Sample Rate to
  **48000** for the lowest-latency AAudio path.
- Windows: WASAPI exclusive mode is selected automatically when supported by
  the audio device; the implementation falls back to shared mode otherwise.
- visionOS: same Objective-C++ source as iOS — set
  `CMAKE_SYSTEM_NAME=visionOS` and the visionOS SDK in CMake.
