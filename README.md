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
| `.github/workflows/` | Csound itself: built from source on GitHub Actions for macOS, Windows, Android and iOS, fetched from npm for WebGL | `Runtime/macOS/`, `Runtime/Win64/`, `Runtime/Android/<abi>/`, `Runtime/iOS/`, `Runtime/WebGL/` |

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

## Building Csound itself

`Update Csound` builds Csound from source for the four native platforms and opens a pull
request on the package with the binaries. Run it from the Actions tab.

What gets built is in [`.github/csound.yml`](.github/csound.yml): a tag pins the package to
a release, a branch plus `follow: true` has a nightly run rebuild whenever that branch moves.
The workflows live here rather than in the package because a Unity package installed from a
git URL is cloned whole, and a `.github` folder there would land in every user's
`Library/PackageCache`.

All of Csound's own I/O modules are turned off (`USE_JACK`, `USE_PORTAUDIO`, `USE_PORTMIDI`,
`USE_IPMIDI`, `USE_COREMIDI`, `USE_AUDIOUNIT`, `USE_ALSA`, `USE_PULSEAUDIO`, `USE_PIPEWIRE`),
and the opcode directory points at a path that does not exist. CsoundUnity does its own audio
and MIDI, and a library that cannot open a device of its own, and does not go looking for
plugins next to a system-wide Csound install, is a library that behaves the same on a
developer's machine and on a user's.

### The token

The last job writes into the package, which a repository's own `GITHUB_TOKEN` cannot do.
It uses a secret named **`PACKAGE_TOKEN`**: a classic personal access token with the `repo`
scope, belonging to someone with write access to the package. Fine-grained tokens do not
work here, as they only reach repositories owned by whoever creates them.

Nothing is merged automatically. The pull request still has to be checked, and on Android
and iOS that means hearing the samples on a real device.

### WebGL

WebGL is not built from source: the browser build is published on npm as `@csound/browser`,
already compiled to WASM, and the workflow fetches it. It carries its own version number —
7.0.0 there is not built from the tag the other platforms compile, and neither follows the
other — so `.github/csound.yml` configures it separately, under `browser`.

What the workflow does beyond downloading: it strips the ES module exports from the end of
`dist/csound.js`. A jspre is concatenated into Unity's `framework.js`, which is not a module,
and a single `export` stops the whole file from parsing — the build then fails at load with
"Unable to parse Build/<name>.framework.js", a message that points at compression rather than
at this.

### Not covered

**visionOS**, which has never been built from source and ships a copy of the iOS libraries.

## Cutting a release

Two workflows, run in order, because a tag has to name a commit that actually landed.

**Release — prepare** writes the version into `package.json`, `CsoundUnity.cs` and the
Doxyfile, dates the changelog section, points the install instructions in `importing.md` at
the new tag, builds the API documentation, and opens a pull request on the package. Nothing
it does survives closing that pull request.

Merge it, then **Release — publish**: it updates the documentation site, tags the merged
commit, and drafts the release with the notes taken from the changelog. The release is a
**draft** — a person publishes it.

Both start with `dry_run` on: they run every check, show what they would do, and change
nothing. The checks are the point, so they are worth listing:

- a version that is already tagged is refused, in both workflows. Unity resolves a package
  URL ending in `#vX` to the tag, so moving one hands different contents to everyone who
  pinned that version. Releases are added, never rewritten.
- the changelog section has to exist already, headed `## [version] - unreleased`. The
  workflow dates it; it does not write release notes.
- Doxygen has to finish with **zero warnings**, or the release stops.
- every `#v…` in `importing.md` has to end up pointing at the new tag; one left behind
  fails the run.

The site keeps `docs/v3/` and its docsify `index.html`: only the pages copied from
`Documentation~` and the generated `docs/html/` are replaced.
