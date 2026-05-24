# VoxStick firmware v0.1.5

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `b4c9c317e45feca898c204fa0032a685063a753b445e3b96899f36da2062384a` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.5` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.5).
It updates the BtnA tap shortcut to `Left Ctrl+F12`, matching the WeType setup
docs for both macOS and Windows. Long-press BtnA still sends `Enter`.
