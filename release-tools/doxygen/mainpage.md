# CsoundUnity API Reference {#mainpage}

The generated reference for the public API of CsoundUnity's `Runtime` assemblies: every class,
property, method and event, with the documentation written alongside the code.

This is a reference, not a guide. For how to use the package — importing it, a first sound, channels,
audio paths, Timelines, platforms — start from the [documentation](../#/), which covers the same
ground in the order you need it.

### Most code touches only a few types ###

- CsoundUnity — the component itself, and nearly everything you will call
- CsoundUnityChild — one named audio channel on its own GameObject
- CsoundChannelController — a channel as parsed from the csd
- CsoundUnityPreset — channel values saved and recalled

The rest is reachable from **Namespaces** and **Classes** above.

Types marked `internal` are not listed: they are implementation detail and may change between
releases without notice.
