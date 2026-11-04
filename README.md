# ohmystick

[English](README.en.md)

M5Stack StickS3（ESP32-S3）的小型啟動器韌體：一支隨身的語音輸入棒。按住前鍵說話，Mac 上的本機 Whisper 轉成文字並自動貼上；沒有電腦在旁邊時，也能先錄成語音筆記，之後回到電腦旁自動補傳。

從 [voxstick](https://github.com/openbrt/voxstick)（openbrt，MIT）分支而來，原本的 USB 麥克風功能完整保留。

## 六個功能

開機後是兩欄格狀的選單，六個功能一頁看完。**側鍵**切換選中的格子，**前鍵**進入，**側鍵**離開。畫面 15 秒後變暗、30 秒後熄屏。

| 功能 | 做什麼 |
|---|---|
| **USB 麥克風** | 原版 voxstick：同時是 USB 麥克風和 HID 鍵盤。單擊前鍵送 `Ctrl+F12`、長按送 `Enter`；平放（螢幕朝上）自動靜音 |
| **即時聽寫** | 按住前鍵說話，錄音經藍牙串流到 Mac，本機 Whisper 轉寫，文字自動貼到游標處 |
| **語音筆記** | 同上，但沒有 Mac 也能錄：錄音存在 Stick 的快閃記憶體（12 筆、每筆最長 30 秒），回到畫面且 Mac 在線時自動補傳，文字進收件夾，不貼上 |
| **簡報翻頁** | 藍牙翻頁筆：前鍵下一頁、側鍵上一頁 |
| **沙漏** | 倒數計時：側鍵選 1／3／5／10／15／30 分鐘，前鍵開始與暫停，沙子從上漏到下；也可以把 Stick 立著翻過去開始；時間到螢幕閃「時間到」並輕響三聲 |
| **狀態** | 白話的狀態頁：連線、待傳筆記、電量、是否曾自動重開，附配對入口 |

USB 麥克風、簡報翻頁、沙漏沒有「離開」的側鍵動作，改為**按住前鍵約 2.5 秒**回選單。

## 需求

- M5Stack StickS3
- 即時聽寫與語音筆記需要一台 **Apple Silicon 的 Mac**，跑 `tools/btmic/` 的小程式（Swift）與 [`mlx_whisper`](https://github.com/ml-explore/mlx-examples/tree/main/whisper)（需要 ffmpeg）。預設模型是台灣華語專用的 [Breeze-ASR-25](https://huggingface.co/MediaTek-Research/Breeze-ASR-25)
- 自己編譯韌體需要 ESP-IDF 5.5.1

## 編譯與刷機

```sh
. $IDF_PATH/export.sh
idf.py -B build-menu -D SDKCONFIG=build-menu/sdkconfig -DVOX_BLE_JUST_WORKS=0 build
```

`-DVOX_BLE_JUST_WORKS=0` 要明確帶（預設已是 0，但 CMake 會快取舊值）。0 是安全的配對碼配對，1 是舊版免確認配對，不建議。

1. USB-C 接上電腦，**按住側邊電源鍵約 2 秒**，綠色 LED 開始閃就是下載模式。
2. 燒錄：
   ```sh
   idf.py -B build-menu -D SDKCONFIG=build-menu/sdkconfig -p /dev/cu.usbmodemXXXX flash
   ```
   這個版本用自訂分區表（`partitions.csv`，含 3 MB 的 `inbox` 給語音筆記），第一次一定要連分區表一起寫，`idf.py flash` 會處理。藍牙配對資料在 NVS，不會被洗掉。
3. 拔掉 USB，**按住電源鍵約 6 秒**關機，**雙擊電源鍵**開機，再插回 USB。

> 原作者的網頁安裝器和 Releases 只燒原版 voxstick，沒有啟動器、即時聽寫和語音筆記。要這個分支請自己編譯。

晶片救援（不用按鍵）見 [`tools/trigger-download.sh`](tools/trigger-download.sh)。

## Mac 端小程式（即時聽寫、語音筆記）

```sh
tools/btmic/build.sh           # 編譯並簽名 out/btmic.app
tools/btmic/install-agent.sh   # 登入時自動啟動、當掉自動重啟（或用 run.sh 手動跑）
```

- 第一次執行需要給 `btmic.app`「藍牙」與「輔助使用」權限（後者才能自動貼上）。重新編譯後如果沒有穩定簽名，輔助使用的授權要重新加回。
- 詞彙檔 `~/.btmic-vocab.txt`（範本 `tools/btmic/vocab.example.txt`）會當作辨識提示，對藥名、縮寫有幫助。
- 記錄在 `~/.btmic.log`，只寫字數，不寫內容（除非設 `BTMIC_LOG_TEXT=1`）。
- 協定、環境變數、踩過的坑：[`docs/BT-MIC.md`](docs/BT-MIC.md)

## 配對與安全

- 配對用**螢幕上的 6 位數配對碼**，連線加密、需要驗證。
- 已經有配對裝置時，Stick **鎖定，拒絕新的配對**。要加新裝置：進「狀態」頁，**長按前鍵 2 秒**，開放 60 秒的配對視窗，這段時間螢幕會顯示配對碼。
- 沒有通過驗證的連線與未驗證的舊配對會被拒絕或清除。

## 隱私

- 音訊只在你的 Mac 上處理：沒有雲端辨識，沒有外送請求。
- 辨識後的文字放在你的家目錄：`~/btmic-inbox.md`（給人讀）、`~/btmic-inbox.jsonl`（給 agent 取件）。
- **選用、預設關閉**：設定 `~/.btmic-cloud-dir` 後，每筆語音筆記會多存一份獨立檔案到你指定的同步資料夾（例如 iCloud Drive）。開啟前請先想過內容是否可以離開這台 Mac，見 [`docs/NOTE-CLOUD.md`](docs/NOTE-CLOUD.md)。

## 語音筆記的收件夾

補傳完成的文字會成為一筆待處理工作，任何 agent 或腳本都用同一組命令取件，要換處理的 agent 只需換呼叫端：

```sh
tools/btmic/inbox list
tools/btmic/inbox claim --by 名稱
tools/btmic/inbox done <id> --result 文字
```

資料格式與標準流程見 [`docs/INBOX.md`](docs/INBOX.md)。筆記內容是資料，不是指令，agent 不應該照著執行。

## 狀態頁怎麼讀

摘要頁用大字給結論（狀態良好、請留意、有問題），下面五行是連線、待傳筆記、電量、新裝置、本機。前鍵短按切到詳細頁。如果顯示「曾自動重開」，**先拍照再按前鍵**，上面的重開機代碼可以對回原因。細節見 [`docs/DIAG.md`](docs/DIAG.md)。

## USB 麥克風（原版 voxstick）

Stick 是一個複合 USB 裝置，電腦同時看到一支 16 kHz 麥克風和一把 HID 鍵盤，免驅動、免 App，macOS、Windows、Linux 都能用。

| 動作 | 輸出 | 用途 |
|---|---|---|
| 單擊前鍵（< 600 ms） | `Left Ctrl + F12` | 切換語音輸入，或綁定給任何支援 Ctrl+F12 的工具 |
| 按住前鍵（≥ 600 ms） | `Enter` | 送出口述的訊息 |
| 開機時按住前鍵 | （無） | 重開進 ROM 下載模式，用來安全重刷 |

平放（螢幕朝上）時，IMU 會自動把麥克風靜音。上游的[網頁設定頁](https://openbrt.github.io/voxstick/config.html)可以改自動靜音與單擊、長按的快捷鍵。本韌體沿用同樣的 USB 識別碼與設定協定，原則上可用，但我沒有實際用這個頁面測過。

## 文件

| 文件 | 內容 |
|---|---|
| [`docs/BT-MIC.md`](docs/BT-MIC.md) | 藍牙錄音協定、Mac 端程式、環境變數、踩過的坑 |
| [`docs/DIAG.md`](docs/DIAG.md) | 狀態頁與重開機代碼怎麼讀、字型產生規則 |
| [`docs/INBOX.md`](docs/INBOX.md) | 語音筆記收件夾與 agent 取件介面 |
| [`docs/WIFI-PLAN.md`](docs/WIFI-PLAN.md) | Wi-Fi 聽寫計畫（草案，經 Tailscale 送到 Windows 電腦） |
| [`docs/NOTE-CLOUD.md`](docs/NOTE-CLOUD.md) | 筆記上雲的研究與選用的雲端資料夾複本 |
| [`SESSION-NOTES.md`](SESSION-NOTES.md) | 上游 voxstick 的開發紀錄（電源晶片、ES8311 等問題） |

## 授權與致謝

- 本專案以 [MIT](LICENSE) 授權，繼承自上游 voxstick。
- 介面中文字使用 [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font)（SIL OFL-1.1），見 [`main/ZH_FONT_LICENSE.md`](main/ZH_FONT_LICENSE.md)。
- Breeze-ASR-25 為聯發創新基地的開源模型（Apache 2.0）。
- USB UAC 骨架參考 [atomic14/esp32-usb-uac-experiments](https://github.com/atomic14/esp32-usb-uac-experiments)；StickS3 電源晶片與編解碼器初始化參考 [m5stack/M5Unified](https://github.com/m5stack/M5Unified) 與 [m5stack/M5GFX](https://github.com/m5stack/M5GFX)。
- 使用 Espressif 的 `usb_device_uac`、`esp_codec_dev` 與 `espressif2022/bmi270` 元件。
