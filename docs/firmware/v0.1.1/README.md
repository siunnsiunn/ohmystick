# VoxStick firmware v0.1.1

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `45d9130b0f3d05333a2e47b2c7914cba1b5ce4fc4d7093b7982cc66a0e819f9b` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.1` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.1).
It fixes the M5PM1 L3B rail control polarity for StickS3. L3B powers the
LCD/MIC/SPK rail; on StickS3 PYG2 must be driven low to enable it.
