# Wi-Fi 聽寫計畫（草案）

讓 Stick 不靠 USB 線、也不靠電腦藍牙，經 Wi-Fi 把錄音送到診所的 Windows 電腦（CLINIC），
由 [`tools/winvoice`](../tools/winvoice/README.md) 送雲端轉寫後貼到游標處。

狀態：**計畫，尚未實作**。各階段都要刷到實機上測過才算完成。

## 背景

- CLINIC：i3-4350、4 GB、Windows 10，沒有藍牙，跑不動本機 Whisper；已裝 Tailscale。
- 手邊的 USB 線不能傳資料，電腦認不到 Stick，USB 麥克風模式用不了。
- Stick（StickS3，ESP32-S3）有 Wi-Fi、8 MB 快閃記憶體、8 MB PSRAM。

## 架構

```
Stick（新 App「Wi-Fi 聽寫」）
  前鍵開始／停止 ─▶ 錄音（IMA ADPCM，沿用 BT-MIC 幀格式）
                  ─▶ Wi-Fi ─▶ Tailscale（WireGuard）─▶ CLINIC:winvoice
CLINIC（winvoice 加上網路接收端）
  組成 WAV ─▶ Groq / Azure ─▶ 貼到游標處
          └▶ 狀態（BUSY / DONE / ERR / PING）回傳 Stick 螢幕
```

設計原則：

- **沿用 [`BT-MIC.md`](BT-MIC.md) 的協定**：音訊幀（172 位元組、20 ms、帶預測值與索引）、開始／結束幀、
  控制位元組與心跳、離線補傳要帶錄音編號的確認規則，全部不變，只把傳輸管道從 BLE GATT 換成 TCP。
  兩端的編碼、解碼和收件匣邏輯可以重用。
- **雲端金鑰只放電腦**。Stick 不直接打 Groq / Azure，弄丟 Stick 也不洩漏 API 金鑰。
- **按鈕事件走網路**：不再需要鍵盤的 `Ctrl+F12`。長按前鍵送 Enter 改為一個控制訊息，由 winvoice 送出。
- **只在「Wi-Fi 聽寫」畫面開 Wi-Fi**，離開就關，省電。

### 為什麼走 Tailscale

| | 區網直連 | Tailscale |
|---|---|---|
| 診所 Wi-Fi 是訪客網路、和電腦隔開 | 不能用 | 能用（只要能上網） |
| 加密與身分驗證 | 要自己做（配對、金鑰交換、AES-GCM，電腦多裝 `cryptography`） | WireGuard 處理 |
| 找到電腦 | 要做 UDP 廣播搜尋 | 固定 `100.x.x.x` 或 MagicDNS 名稱 |
| 不在診所時補傳語音筆記 | 不能 | 能 |
| 風險 | 自製加密的正確性 | ESP32 上的 Tailscale 是社群元件（[MicroLink](https://github.com/CamM2325/microlink)），非官方 |

所以先用一個小階段驗證 MicroLink 在 Stick 上可不可靠；可靠就採用 Tailscale，不可靠就退回區網直連（見附錄）。

## 階段

每個階段結束時，Stick 和 winvoice 都要能照常使用，既有的六個 App 不受影響。

### 第 0 步：準備（使用者）

- [ ] 一條 **USB-A 轉 USB-C 傳輸線**（第一次刷韌體一定要；做完第 5 階段之後才不用）。
- [ ] Tailscale 後台權限：CLINIC 登入的帳號要能建立預先授權金鑰（auth key）。
- [ ] CLINIC 的 Tailscale 位址與名稱：`tailscale ip -4`、`tailscale status`。
- [ ] 診所 Wi-Fi 名稱、密碼，確認有 **2.4 GHz**（ESP32-S3 不支援 5 GHz）。

### 階段 T：Tailscale 試水溫

目標：用最小的測試韌體證明 Stick 能加入 tailnet 並穩定連到 CLINIC。

- 韌體：Wi-Fi 連線參數與 Tailscale auth key 先編譯時寫入（只用於測試，不進 git）。
  加入 tailnet 後每秒送一個 TCP 測試封包到 CLINIC，螢幕顯示狀態、往返時間、直連或中繼。
- 電腦：一個接收測試封包並回送的小腳本。
- 測量：連線所需時間、30 分鐘內的斷線次數、韌體大小、剩餘記憶體、耗電。

**通過標準**：開機 30 秒內連上；30 分鐘不斷線；以每秒 50 幀、每幀 172 位元組送 60 秒不掉資料；
韌體加上 MicroLink 後不超過預定的程式分區。

**不通過**：改走附錄的區網直連方案，階段 1 以後照做，只有傳輸層不同。

### 階段 1：Wi-Fi 基礎與設定

- **分區表**：改成可無線更新（OTA）的配置，例如 `otadata` 加兩個約 2 MB 的程式分區，收件匣維持 3 MB
  （約 `0x10000 + 2 × 0x200000 + 0x300000 = 0x710000`，在 8 MB 內）。實際程式分區大小依階段 T 量到的韌體大小決定。
  **這次刷入會清掉收件匣**，刷之前要先把語音筆記傳完。
- **Wi-Fi 設定**：Stick 開一個暫時的熱點 `ohmystick-setup`，手機連上後在網頁輸入 Wi-Fi 名稱、密碼、
  CLINIC 位址、Tailscale auth key，存進 NVS。不需要電腦，也不需要 USB。
- **新 App「Wi-Fi 聽寫」**：選單目前一頁六格，第七格要改成翻頁，或合併／移除一個 App（待決定）。
- **狀態頁**：顯示 Wi-Fi 訊號、IP、Tailscale 連線狀態（直連或中繼）。

### 階段 2：即時聽寫

- 韌體：按前鍵開始錄音並以 TCP 串流，放開後送結束幀；顯示 READY / REC / BUSY / DONE / ERR / NO LINK，
  心跳規則同 BT-MIC（6 秒內沒收到心跳就算斷線）。
- winvoice：新增網路接收模式（只聽 Tailscale 介面），解 ADPCM、組 WAV，交給既有的轉寫與貼上流程，
  狀態回傳 Stick。安裝腳本加上 Windows 防火牆規則，只允許 Tailscale 網段連入。
- Tailscale 存取規則（ACL）：只允許 Stick 這個節點連到 CLINIC 的 winvoice 埠。

**通過標準**：在 CLINIC 的記事本裡，按前鍵說一句話、再按一次，文字在約 3 秒內出現；連續使用 30 分鐘不出錯。

### 階段 3：語音筆記走 Wi-Fi

- 沒連上時錄音存進 Stick 收件匣；連上後自動補傳，winvoice 把文字寫進收件夾檔案，不貼上。
- 沿用 BT-MIC 的補傳確認規則：DONE / ERR 帶錄音編號，相符才刪 Stick 上的那一筆。

### 階段 4：無線更新（OTA）

- winvoice 或一個小指令把新韌體經 Tailscale 推到 Stick；Stick 驗證後重開，開機自檢失敗就退回舊版。
- 完成後，日後更新不再需要 USB 線。

## 風險與因應

| 風險 | 因應 |
|---|---|
| MicroLink 不穩或太大 | 階段 T 先驗證；不行就走區網直連 |
| Wi-Fi 耗電 | 只在 Wi-Fi 聽寫畫面開 Wi-Fi；階段 T 量測，必要時加上閒置自動關閉 |
| Wi-Fi 與 BLE 共用天線 | ESP32-S3 支援共存；Wi-Fi 聽寫畫面暫停 BLE 廣播以降低干擾 |
| auth key 存在 Stick 上 | 專用金鑰、加 tag、ACL 只開一個埠；Stick 遺失時在後台移除該節點 |
| 中繼（DERP）延遲 | 語音 64 kbps，延遲幾百毫秒對聽寫影響不大 |
| 每階段都要實機測試 | 每階段做成可單獨刷入、可退回的版本，由使用者在診所測試回報 |

## 附錄：區網直連（備案）

只有在階段 T 不通過時採用。需要先確認診所 Wi-Fi 和 CLINIC 在同一個網路：

1. CLINIC 執行 `ipconfig`，記下 IPv4 位址（例如 `192.168.1.23`）。
2. CLINIC 執行 `py -m http.server 8000`，Windows 防火牆詢問時允許。
3. 手機連上**診所 Wi-Fi**（關掉行動數據），瀏覽 `http://192.168.1.23:8000`。看得到檔案清單才算通。

通的話，傳輸改為：UDP 廣播找到 winvoice；第一次以 X25519 交換金鑰，Stick 螢幕與電腦各顯示 4 位數確認；
之後每幀以 AES-GCM 加密（ESP-IDF 用 mbedTLS，電腦端加裝 `cryptography`）。

## 待決定

1. 第七個 App 的位置：選單翻頁，或移除／合併哪一個 App。
2. 階段 T 的結果決定走 Tailscale 或區網直連。
3. Wi-Fi 聽寫是否也要支援在 Mac 上用（btmic 加網路接收）；目前只規劃 CLINIC。
