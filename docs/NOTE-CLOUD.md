# NOTE 轉寫後上雲到收件夾：做法研究

現況：NOTE 錄音補傳到 Mac → Breeze 轉寫 → 追加到 `~/btmic-inbox.md` → 回 DONE，Stick 才刪原檔。目標：文字也進到雲端的「收件夾」，手機和別台電腦都看得到。

## 先定三個原則

1. **本機先存、回 DONE，雲端非同步補送。** 雲端失敗不能讓 Stick 的錄音留著重傳（會重複轉寫）。做法：轉寫完先寫本機檔，再把一筆「待上傳」放進 `~/.btmic-outbox/`，背景重試到成功才刪。
2. **每筆有固定的識別碼**（錄音時間＋文字的雜湊），雲端以它去重，所以重送不會變兩筆。
3. **內容是看診口述，視為機敏。** 不放病患可辨識資料的假設不可靠，所以要有傳輸加密（HTTPS）加存取控制，最好再加端到端加密。

## 方案比較

| 方案 | 做法 | 優點 | 缺點 |
|---|---|---|---|
| A. iCloud Drive 資料夾 | btmic 把每筆存成 `~/Library/Mobile Documents/com~apple~CloudDocs/Stick收件夾/日期.md`（追加） | 零伺服器、零金鑰，iPhone 的「檔案」直接看，Apple 帳號本身加密 | 只有 Apple 生態系；無法用網頁或 API 查；不是即時 |
| B. Cloudflare Worker + D1（或 KV） | btmic `POST /note`，帶固定權杖；Worker 寫入資料庫；另做一個唯讀的收件夾網頁（Cloudflare Access 保護） | 跨平台、可查詢、可再接日曆待辦用量等後續功能（選單計畫本來就要這個 Worker） | 要維護；資料存在第三方，需自行加密 |
| C. Google Drive／Docs 附加 | 用 Google API 追加到一份文件 | 你已經在用 Google 工具，Claude 端也讀得到 | btmic 要自己做 OAuth；權杖保管與重新授權麻煩 |
| D. 寄信給自己或建立提醒事項 | AppleScript 呼叫 Mail／Reminders／Notes | 做法最短，直接進手機 | 內容散在各 app，不易整理；寄信是外送 |

## 建議

分兩步，兩步都不用改韌體和協議：

**第 1 步（一晚就能做完）：A。** 在 `saveToInbox` 多寫一份到 iCloud Drive 資料夾，環境變數 `BTMIC_CLOUD_DIR` 指定，沒設就維持現狀。這就達成「上雲＋手機看得到」，而且沒有新的攻擊面。

**第 2 步（要做日曆、待辦、用量時一起做）：B。**
- 端點：`POST /note`，標頭 `Authorization: Bearer <權杖>`，本文 JSON：`{id, recorded_at, received_at, text}`。
- 權杖放在 Mac 的鑰匙圈（`security add-generic-password`），不放檔案、不進 repo。
- Worker 以 `id` 做主鍵 `INSERT OR IGNORE`，重送無害；回 200 才從 outbox 刪除。
- 端到端加密：btmic 用對稱金鑰（鑰匙圈）加密 `text` 再上傳，Worker 只存密文；收件夾網頁在瀏覽器端解密。代價是不能在伺服器端全文搜尋，要搜尋就在客戶端做。
- 收件夾網頁用 Cloudflare Access 限定你的帳號；列表依時間排序，每筆有「已處理」勾選。
- 後續可在 Worker 或 Mac 端加一步：用小模型把口述整理成「待辦／病歷草稿／想法」分類。

## 要先決定的事

- 內容可否離開你的 Mac？若部分筆記含病患資料，建議第 2 步一定要端到端加密，或只在手動標記時才上傳（例如口述開頭說「雲端」）。
- 收件夾要手機看，還是要網頁看？只要手機，A 就夠了。

## 實作點（給之後的我）

- `btmic.swift` 的 `saveToInbox` 已回傳成功與否；新增 `enqueueCloud(id:text:)`，寫入 outbox 檔案後由一個每 30 秒的計時器送出。
- outbox 檔名用 `id`，檔案權限 0600；成功後用 `removeItem` 刪除。
- 記錄只寫字數，沿用 `BTMIC_LOG_TEXT` 的規則。
- 驗收：關網路錄一筆 NOTE 補傳，Stick 仍收到 DONE；恢復網路後雲端出現一筆；重複重送雲端仍只有一筆。

## 已實作：iCloud 資料夾複本（選用，預設關閉）

`btmic` 在存好本機收件夾之後，若設定了資料夾，就多寫一份「一筆一檔」的 Markdown（`note-日期-時間-雜湊.md`，權限 0600）。失敗只記錄，不影響本機儲存與回 DONE。

開啟（擇一）：

```
echo '~/Library/Mobile Documents/com~apple~CloudDocs/Stick收件夾' > ~/.btmic-cloud-dir
launchctl kickstart -k gui/$(id -u)/local.btmic
```

或在 LaunchAgent 的環境變數設 `BTMIC_CLOUD_DIR`。關閉：刪掉 `~/.btmic-cloud-dir`。注意：開啟後語音筆記的內容會進 iCloud，看診口述請走 WHISPER 而不是 NOTE。
