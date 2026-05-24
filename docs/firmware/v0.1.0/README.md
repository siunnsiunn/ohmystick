# VoxStick firmware v0.1.0

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `a78ea87491ff95f782e670c68143fdcb2aff5f1f47d84995b338f7087c54ffe0` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

The binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.0` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.0).
It is kept in the Pages tree so browsers can fetch it from the same origin as
the installer, avoiding cross-origin download failures.
