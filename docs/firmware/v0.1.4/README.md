# VoxStick firmware v0.1.4

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `aa1e2a3803ae6a981207e5faf86c3763fd36632cbfb72c955451b3c7d00c10b4` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.4` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.4).
It refines the low-power LCD mute state: the muted mic icon is drawn in light
gray and the red slash is thinner, so the screen reads as a muted microphone
instead of just a bright red line.
