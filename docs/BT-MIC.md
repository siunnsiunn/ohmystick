# BT MIC 串流（B 方案）規格 v0.1

Stick 的麥克風經 BLE 傳到 Mac 小程式，本機 Whisper 轉文字後貼進目前的輸入框。

和 `BT MIC`（A 方案，只送聽寫快捷鍵、用 Mac 麥克風）是兩回事。選單會新增第四項 `BT REC`。

## 流程

```
按住前鍵 ─▶ Stick 開始錄音（螢幕紅色）─▶ ADPCM 分段經 BLE 通知
放開前鍵 ─▶ Stick 送 end 幀 ─▶ Mac 小程式組成 WAV ─▶ mlx_whisper ─▶ 文字
                                                     └▶ 剪貼簿 + Cmd+V 貼上（可選送 Return）
```

## 為什麼用 IMA ADPCM

- BLE 實際頻寬有限。16 kHz、16 bit PCM 是 256 kbps，太高。
- IMA ADPCM 4 bit，16 kHz 為 64 kbps，兩端實作都只要幾十行，不用額外函式庫。
- 語音品質對 Whisper 足夠；比 8 kHz μ-law 好。
- 日後可換 Opus，協議保留 `codec` 欄位。

## GATT

自訂服務，和既有 HID 服務並存（HID 使用 `0x1812`，互不影響）。

| 項目 | UUID | 屬性 |
|---|---|---|
| Service | `5e3b0000-8a7c-4f1e-9b2d-7c4a1f0e6d10` | |
| `audio_tx` | `5e3b0001-8a7c-4f1e-9b2d-7c4a1f0e6d10` | notify，需加密 |
| `ctrl_rx` | `5e3b0002-8a7c-4f1e-9b2d-7c4a1f0e6d10` | write without response，需加密 |
| `state_tx` | `5e3b0003-8a7c-4f1e-9b2d-7c4a1f0e6d10` | notify，需加密 |

所有特徵都要求加密連線，沿用 HID 已有的綁定（`SM_LVL=2`）。

## 音訊幀（`audio_tx`，小端序）

| 位移 | 大小 | 欄位 |
|---|---|---|
| 0 | 1 | `ver` = 1 |
| 1 | 1 | `type`：1 = 音訊，2 = 結束，3 = 開始 |
| 2 | 2 | `session`：每次錄音遞增 |
| 4 | 2 | `seq`：幀序號，從 0 起 |
| 6 | 1 | `codec`：1 = IMA ADPCM 16 kHz 單聲道 |
| 7 | 1 | 保留 = 0 |
| 8 | 2 | `predictor`：int16，本幀起始預測值 |
| 10 | 1 | `index`：步階索引 0–88 |
| 11 | 1 | 保留 = 0 |
| 12 | 160 | 320 個樣本（20 ms），每位元組兩個，低四位元先 |

- 每幀 172 位元組，需要 ATT MTU ≥ 175（Mac 預設協商得到 185 以上）。
- 一秒 50 幀，約 69 kbps。
- 每幀自帶預測值與索引，掉幀不會讓後面全壞。
- `type` 2 的結束幀沒有 ADPCM 資料（長度 12）。

## 控制（`ctrl_rx`）與狀態（`state_tx`）

- 控制：`{"cmd":"mic_ok"}`、`{"cmd":"busy"}` 等小 JSON，讓 Mac 告訴 Stick 轉譯狀態，螢幕顯示。
- 狀態：`{"ev":"start"|"end","session":n}`。

## 隱私與安全

- 麥克風**只在按住前鍵時錄音**，螢幕紅色；放開即停，藍牙不傳其他時間的音訊。
- 全程本機：Whisper 在 Mac 上跑，沒有網路請求，沒有雲端辨識。
- 連線必須加密綁定；沒有加密的連線無法訂閱 `audio_tx`。
- Mac 小程式不開任何網路埠。
- 貼上需要「輔助使用」權限；沒授權時只複製到剪貼簿，不貼。
- 暫存 WAV 轉譯完立即移到垃圾桶。

## 風險

1. **在已當作 HID 鍵盤連線的裝置上再加自訂服務**：macOS 會快取服務清單，新增服務後要忘記裝置再配對一次。
2. **CoreBluetooth 能不能連到已被系統當 HID 連著的周邊**：理論上可以用 `retrieveConnectedPeripherals(withServices:)`，要實測。
3. **藍牙頻寬與連線間隔**：69 kbps 在 BLE 4.2 以上應該可行，要實測丟幀率。
4. **中英夾雜的辨識品質**：獸醫專有名詞需要提示詞，後續調。

## 階段

| 階段 | 內容 | 可離線測試 |
|---|---|---|
| B0 | Mac 端：ADPCM 編碼與解碼、WAV、Whisper、貼上，用檔案模擬 | 是 |
| B1 | Mac 端 BLE 接收（CoreBluetooth） | 需韌體 |
| B2 | 韌體：自訂服務加 ADPCM 編碼加選單 `BT REC` | 需實機 |
| B3 | 實機整合、丟幀與延遲調校、提示詞 | 需實機 |
