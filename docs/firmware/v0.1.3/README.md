# VoxStick firmware v0.1.3

This directory is served by GitHub Pages for the browser-based installer.

| File | Purpose | SHA-256 |
|---|---|---|
| `voxstick-full.bin` | Merged ESP32-S3 image for one-shot flashing at `0x0` | `ed53a8ec52fb409af70e5a63a763a87c3a2a9c3028e041a5857d850d6ec247f2` |
| `manifest.json` | ESP Web Tools manifest consumed by `docs/install.html` | - |

This binary mirrors the `voxstick-full.bin` asset from the
[`v0.1.3` GitHub release](https://github.com/openbrt/voxstick/releases/tag/v0.1.3).
It changes the StickS3 LCD from a bright full-screen ready fill to a
low-power status display: dim PWM backlight, black background, open/muted mic
icon, and a tiny audio level meter. IMU flat-detect mute is reflected in the
same muted icon as host-side UAC mute.
