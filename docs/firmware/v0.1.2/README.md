# VoxStick firmware v0.1.2

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `fba6b95db31a9e8802c00fbf7f7e48c2927a0e3d45388466ddec8ba4072b3076` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.2` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.2).
It mirrors the M5GFX StickS3 LCD bring-up more closely: M5PM1 GPIO2 is driven
high for the L3B LCD/MIC/SPK rail, the LCD SPI device uses 3-wire mode, and
the ready screen is a full green fill so display power is obvious.
