# VoxStick firmware v0.1.6

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `4a81b4f05b656e0bed65ba5e4bb710182d19109d990089ebd43de14ec272b449` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This build adds persistent browser configuration through `docs/config.html`.
Defaults stay compatible with v0.1.5: tap BtnA sends `Left Ctrl+F12`,
long-press BtnA sends `Enter`, and flat auto-mute remains enabled.
