# BT REC 串流（B 方案）規格 v1.0（已實機驗證）

Stick 的麥克風經 BLE 傳到 Mac 小程式，本機 Whisper 轉文字後貼進目前的輸入框。

和 `BT MIC`（A 方案，只送聽寫快捷鍵、用 Mac 麥克風）是兩回事。選單第三項就是 `BT REC`。

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

所有特徵都要求加密連線，沿用 HID 已有的綁定（`SM_LVL=2`）。初稿的 `state_tx` 沒有實作，也用不到。

服務在 `components/esp_hid/src/nimble_hidd.c` 的 `nimble_hid_start_gatts()` 裡、HID 服務之後註冊（弱符號 `vox_audio_svc_init`），與 HID 並存。

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

## 控制（`ctrl_rx`）

Mac 寫入 1 個位元組，讓 Stick 顯示轉寫狀態：`1` = 轉寫中（BUSY）、`2` = 已貼上（DONE）、`3` = 失敗（ERR，含太短、靜音）。BUSY 最長維持 60 秒，DONE 與 ERR 維持 3 秒。

Stick 畫面：`NO LINK`（未訂閱／未加密／MTU 不足）、`READY`、`REC`、`BUSY`、`DONE`、`ERR`。

結束幀會連送 3 次，因為通知可能掉；Mac 端也有 1.5 秒無資料的逾時保險。單次錄音上限 60 秒。

## 隱私與安全

- 麥克風**只在按住前鍵時錄音**，螢幕紅色；放開即停，藍牙不傳其他時間的音訊。
- 全程本機：Whisper 在 Mac 上跑，沒有網路請求，沒有雲端辨識。
- 連線必須加密綁定；沒有加密的連線無法訂閱 `audio_tx`。
- Mac 小程式不開任何網路埠。
- **只信任 macOS 已配對並連線的裝置**，並把第一次連上的 UUID 記在 `~/.btmic-device`（權限 600），之後只認這個 UUID；沒有掃描備援，所以偽造 `StickS3` 廣播連不上。重新配對後 UUID 變了，刪掉這個檔案即可重新綁定。
- 沒有對音訊幀做簽章：金鑰仍要靠同一條配對通道交換，信任基礎不變，卻讓協議複雜很多，所以不做。
- 預設只貼上，不按 Return；要自動送出需設環境變數 `BTMIC_ENTER=1`。
- 貼上需要「輔助使用」權限；沒授權時只複製到剪貼簿，不貼。
- 暫存 WAV 轉譯完立即移到垃圾桶。

## 實測結果與坑

1. **新增服務後 macOS 的服務快取**：用完整映像刷機（清掉 Stick 配對），Mac 也忘記裝置再重新配對，一次就成功。
2. **CoreBluetooth 看不到 HID 服務**：`retrieveConnectedPeripherals(withServices: [0x1812])` 回傳空，但用自訂服務 UUID 查得到已連線的 Stick（`btmic probe` 可重現）。所以接收端查自訂服務。
3. **隱私權（TCC）**：直接從終端機執行會因為缺少藍牙使用說明而被系統中止。所以包成 `out/btmic.app`（含 `Info.plist`），以 `open` 啟動，權限屬於 app 本身。藍牙與輔助使用都要對這個 app 授權；重新編譯後若失效，到系統設定移除再加入。
4. **頻寬**：4.2 秒與 2.4 秒的錄音丟幀為 0；30 秒以上與遠距離尚未測。
5. **延遲**：放開到文字出現約 3 到 4 秒，主要是 `whisper-medium` 轉寫時間。
6. **辨識**：中文含數字（「心跳每分鐘 120 下」）正確；夾英文的獸醫術語尚未測，之後再調提示詞。

## 使用

```
tools/btmic/build.sh        # 編譯並簽署 out/btmic.app
tools/btmic/run.sh          # 啟動接收（記錄：~/.btmic.log）
pkill -x btmic              # 停止
out/btmic.app/Contents/MacOS/btmic selftest <16k 單聲道 wav>   # 離線自測（不需 Stick）
```

環境變數：`BTMIC_LANG`（預設 `zh`）、`BTMIC_MODEL`（預設 `mlx-community/whisper-medium`）、`BTMIC_ENTER=1`。

## 階段

| 階段 | 內容 | 狀態 |
|---|---|---|
| B0 | Mac 端 ADPCM、WAV、Whisper、貼上 | 完成，離線自測通過 |
| B1 | Mac 端 BLE 接收 | 完成，實機通過 |
| B2 | 韌體：自訂服務、ADPCM、選單 `BT REC` | 完成，實機通過 |
| B3 | 長錄音、遠距離、術語提示詞調校 | 未做 |
