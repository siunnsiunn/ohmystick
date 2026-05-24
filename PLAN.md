# voxstick implementation plan

Three independently verifiable steps. Each step's output is usable on its own
even if the next step never lands.

## Step 1 — USB UAC bring-up

**Goal**: macOS `System Settings > Sound > Input` shows `StickS3-Mic`,
QuickTime records clean voice from the ES8311 mic.

- PlatformIO project with `framework = espidf`, `board = esp32-s3-devkitc-1`
- Pull `espressif/usb_device_uac` component via `idf_component.yml`
- I2S in **standard** (not PDM) mode against the ES8311 codec at:
  - MCLK=18 BCLK=17 WS=15 DIN=16 (StickS3 ES8311 mic-in path)
- ES8311 init via I2C0 (SDA=47 SCL=48) — drop in `esp_codec_dev` component or
  inline the register sequence used by M5Unified
- Mic only for v1. Skip speaker / power amp.
- 16 kHz / mono / 16-bit (Whisper-native; smaller MTU, less USB churn)

**Acceptance**
- `system_profiler SPAudioDataType | grep -A5 StickS3` shows the device
- 30 s continuous recording in QuickTime, no dropouts, voice intelligible

**Risks**
- ES8311 init order under PMIC AXP2101 — may need to power codec rail before
  I2C probe
- StickS3 PA defaults to ON → audible click; mute via M5PM1 register or leave
  the speaker amp disabled
- TinyUSB endpoint budget — UAC alone uses 2 EPs, fine here

## Step 2 — HID PTT + end-to-end dictation

**Goal**: hold BtnA → speak → release → transcribed text lands at the cursor
in whatever app is focused.

- USB descriptor → composite UAC + HID Consumer Control (reference
  [Alexaznavour/micemul](https://github.com/Alexaznavour/micemul))
- BtnA press / release → HID report sending `KEY_F19` (or vendor-defined usage
  if F19 conflicts; Karabiner-Elements as fallback)
- BtnB → `KEY_F20` reserved for future modes
- LCD: minimum viable indicator — red dot in top-right while button is held
- Mac side: install [VoiceInk](https://github.com/Beingpax/VoiceInk),
  - input device = `StickS3-Mic`
  - PTT hotkey = F19
  - inject to focused app

**Acceptance**
- In Notes / Slack / terminal, hold BtnA, dictate, release → text appears
  within ~3 s
- Macro 30-min session has no dropped audio, no missed PTT events

**Risks**
- macOS first-time HID approval prompt — unavoidable (Ventura+ security)
- VoiceInk hotkey support for high F-keys — fallback via Karabiner remap
- Composite descriptor endpoint layout on ESP32-S3 native USB (6 EPs total)

## Step 3 — LCD UX + reverse channel

**Goal**: stick becomes a status panel — `recording → transcribing → preview`,
with optional double-tap-to-toggle-language and IMU wake.

- Add a third interface to the composite: USB CDC, a simple line-oriented
  channel for Mac → device messages
- Mac daemon (Swift / Node, ~150 LOC):
  - either: tail VoiceInk's log / observe its state notifications and forward
  - or: implement the PTT loop directly (CGEventTap → AVAudioEngine →
    whisper.cpp → CGEventPost), giving fine-grained status control
- Firmware draws three states on the 240×135 LCD:
  - `recording` — red waveform / level meter
  - `transcribing` — spinner
  - `done "<text>"` — text bubble for 2 s
- Optional: double-tap BtnA = swap language, long-press BtnB = re-dictate
  cached PCM buffer, IMU wake

**Risks**
- Composite three-EP layout (UAC iso + HID intr + CDC bulk) is tight on S3
- VoiceInk doesn't expose IPC for state — would force "implement own PTT"
  branch, which is more work but more controllable

## Out of scope (for now)

- WiFi / wireless operation — adds host-side daemon and battery management
- On-device ASR — whisper-tiny on ESP32-S3 is borderline, not worth the
  complexity when Mac-side whisper.cpp is fast and free
- Cloud ASR — privacy regression vs. local whisper.cpp
