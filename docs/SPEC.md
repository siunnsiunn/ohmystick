# claude-approval-stick 規格 v0.1

用 M5Stack StickS3 的實體按鍵，回答 Claude Code 的權限請求（終端機版，例如 Ghostty）。

## 目標與非目標

- 目標：Claude Code 要執行工具時，Stick 顯示摘要，按鍵決定允許或拒絕。
- 非目標：不放寬現有權限規則；不做遠端、無線、雲端；不取代 `~/.claude/settings.json` 的 allow／deny。

## 流程

```
Claude Code ──PermissionRequest──▶ hook/permission_request.sh
                                      │ 一行 JSON（USB 序列埠）
                                      ▼
                                   StickS3：顯示摘要、倒數
                                      │ 按鍵
                                      ▼
                          一行 JSON 回答 ──▶ hook 輸出 allow／deny
```

## 失敗安全（最重要）

hook **只有在收到 id 相符、格式正確的明確回答時才輸出決定**。其他情況一律不輸出、exit 0，Claude Code 照常在終端機跳確認。

| 情況 | 結果 |
|---|---|
| 沒接 Stick、序列埠不存在 | 退回終端機確認 |
| 逾時（預設 30 秒）無按鍵 | 退回 |
| 回答 id 不符、不是 JSON | 忽略，繼續等，到逾時退回 |
| 危險指令只單按「允許」 | 退回（需長按確認） |
| 指令被截斷顯示 | 視為危險，同上 |
| 缺 jq 或 timeout | 退回 |

絕不自動允許。

## 序列協議（每行一個 JSON，UTF-8，115200）

主機到 Stick：

```json
{"v":1,"type":"ask","id":"<uuid>","tool":"Bash","summary":"git push origin main","danger":true,"truncated":false,"ttl":30}
```

Stick 到主機：

```json
{"v":1,"id":"<同上>","decision":"allow","confirmed":true}
{"v":1,"id":"<同上>","decision":"deny"}
```

- `confirmed:true` 只在長按時送出。`danger` 為真時，hook 沒看到 `confirmed:true` 就不採用 allow。
- Stick 收到新請求時，舊請求作廢（同一時間只處理一個）。
- Stick 逾時（`ttl`）後不回答。

## 按鍵

| 按鍵 | 一般 | 危險或截斷 |
|---|---|---|
| 前鍵 BtnA（GPIO11） | 單按＝允許 | 單按無效，**長按 1.5 秒**＝允許（`confirmed`） |
| 側鍵 KEY2（GPIO12） | 單按＝拒絕 | 單按＝拒絕 |
| 電源鍵 | 不使用 | 不使用 |

## 螢幕（135×240）

- 上方：工具名稱；危險或截斷時整條紅色並寫「長按確認」。
- 中間：摘要自動換行，最多 6 行；被截斷時末行顯示「…已截斷」。
- 下方：倒數條與按鍵提示。
- 平常（沒有請求）：待機畫面，顯示已連線與電量，螢幕可自動變暗。

## hook 行為細節

- 摘要在主機端先處理：遮蔽 `sk-`、`ghp_` 開頭的金鑰、`Bearer` 權杖、`token=`／`password=`／`api_key=` 之後的值；移除控制字元；超過 110 字截斷。
- 遮蔽只處理「像金鑰的字元」（英數與 `. _ ~ + / = -`），`$ ( ) ; | & > <` 與反引號永遠不遮，避免把可執行內容藏起來。
- **只要顯示內容與實際指令不同（有遮蔽或截斷），一律視為危險，需長按確認**；請求裡帶 `redacted:true` 讓螢幕標示。
- 危險判斷（Bash）：`rm`、`sudo`、`dd`、`chmod`、`chown`、`kill`、`launchctl`、`git push|reset|clean|checkout --|branch -D`、`wrangler deploy|delete|publish`、`curl|wget … | sh`、寫入 `/etc`、`/usr`、`/System`、`--force`、`-rf`。
- 其他工具：Write／Edit 顯示檔案路徑；其餘顯示參數名稱，不顯示內容。

## 啟用方式（尚未套用）

權限相關設定只寫進 `~/.claude/settings.json`，由你決定何時加：

```json
{
  "hooks": {
    "PermissionRequest": [
      { "hooks": [ { "type": "command", "command": "STICK_DEV=/dev/cu.usbmodemXXXX /Users/Jung/Documents/1_專案/工具開發/claude-approval-stick/hook/permission_request.sh", "timeout": 40 } ] }
    ]
  }
}
```

`timeout` 要大於 `STICK_TIMEOUT`（預設 30）。

## 待實機驗證

1. `PermissionRequest` hook 等待期間，終端機的確認畫面是否同時顯示、先按哪邊是否都有效。
2. 多個請求連續湧入（平行工具呼叫）時，是否序列化。
3. Stick 韌體 USB 序列埠在 macOS 的埠名是否穩定（需要 CDC 介面；現有 voxstick 沒有）。
4. 韌體要獨立專案還是併入 voxstick 當第三模式：先做獨立，驗證後再決定。

## 測試

`tests/run.sh`：hook 與假 Stick 用 FIFO 互通，涵蓋允許、拒絕、危險確認、截斷、沉默、錯誤 id、亂碼、無裝置、金鑰遮蔽，共 12 項。無需硬體。
