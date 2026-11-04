# winvoice：Windows 上用 Stick 聽寫（Azure 語音／Groq Whisper）

給沒辦法跑本機 Whisper 的 Windows 電腦用，例如診所那台 i3、4 GB 的 Windows 10。
按一下 Stick 前鍵開始錄音，再按一下停止；錄音送到雲端（Azure 語音或 Groq）轉成繁體中文，
1～2 秒後貼到游標處。

```
前鍵（Ctrl+F12）─▶ 嗶（高音），開始錄音
前鍵（Ctrl+F12）─▶ 嗶（低音），停止 ─▶ HTTPS 送 Azure／Groq ─▶ 文字貼到游標處
```

- Stick 用原本的 **USB MIC**（USB 麥克風）模式，前鍵單擊預設就是 `Ctrl+F12`，韌體不用改。
- 不用 Speech SDK，也不用 Visual C++ 執行階段，只要 Python 和一個套件 `sounddevice`。
  執行時約佔 40～60 MB 記憶體，CPU 幾乎不用。
- 為什麼不用 Windows 內建的 Win+H：Windows 10 的聽寫不支援繁體中文（台灣），
  Windows 11 才有。winvoice 用的是同一家（Azure）的辨識，Windows 10 也能用。

## 兩種辨識方式

| `backend` | 方案 | 單次上限 | 詞彙表 | 費用 |
|---|---|---|---|---|
| `short`（預設） | Azure 短音訊 REST API | 60 秒 | 不支援 | 免費方案 F0 每月 5 小時 |
| `fast` | Azure 快速轉寫 API | 本程式設 `max_seconds` | 支援 `vocab.txt` | 要付費方案 S0，依秒計費；F0 不提供 |
| `groq` | Groq 的 Whisper large-v3 | 本程式設 `max_seconds` | `vocab.txt` 當提示詞 | 免費方案每天約 28,800 秒（8 小時）、每天 2,000 次；不足 10 秒以 10 秒計 |

- `short`：Azure 原生台灣繁中，免費額度少。
- `groq`：免費額度最多，有提示詞；但它是原版 Whisper（不是 Breeze），偶爾輸出簡體，
  winvoice 會用 OpenCC 轉回繁體；靜音時 Whisper 可能「聽」出「字幕由 Amara.org 提供」之類的字，
  winvoice 會丟掉這類短句。
- 兩個都申請，同一段話用 `--test` 各跑一次，比較哪個準。

額度與價格常變動，以 [Azure 定價頁](https://azure.microsoft.com/pricing/details/cognitive-services/speech-services/)
和 [Groq rate limits](https://console.groq.com/docs/rate-limits) 為準。

## 安裝

### 1. 申請 API 金鑰（Azure、Groq 擇一或都申請）

**Groq**：到 [console.groq.com](https://console.groq.com) 註冊，在 API Keys 建立一把金鑰（`gsk_` 開頭），
填進 `groq_key`，`backend = groq`。

**Azure**：

1. 到 [Azure 入口網站](https://portal.azure.com) 建立「語音服務（Speech service）」資源。
2. 區域選近的，例如 `East Asia`（香港，代號 `eastasia`）；定價層選 `Free F0`
   （一個訂用帳戶只能有一個 F0）。
3. 建好後在「金鑰與端點」複製 **金鑰 1** 和 **位置／區域**。

### 2. 安裝 Python

1. 從 [python.org](https://www.python.org/downloads/windows/) 下載 Windows 64 位元安裝程式（3.12 或 3.13）。
2. 安裝時勾選 **Add python.exe to PATH**，並保留 **py launcher**。
3. 開「命令提示字元」：

```bat
py -m pip install sounddevice opencc-python-reimplemented
```

（`opencc-python-reimplemented` 是純 Python 的簡轉繁，只有 `groq` 用得到。）

### 3. 設定

把 `tools/winvoice` 資料夾複製到電腦上（例如 `C:\winvoice`），然後：

1. 複製 `winvoice.example.ini` 為 `winvoice.ini`，填入金鑰（Azure：`key`、`region`；Groq：`groq_key`）並設定 `backend`。
2. 用 `fast` 或 `groq` 時，複製 `vocab.example.txt` 為 `vocab.txt` 再改成自己的詞。
3. Stick 插上 USB、切到 **USB MIC**，**拿直**（平放會自動靜音）。
4. 確認抓得到 Stick 的麥克風，並試錄 5 秒：

```bat
cd C:\winvoice
py winvoice.py --list-devices
py winvoice.py --test
```

`--test` 會印出辨識結果和花費秒數。名稱裡有 `StickS3` 的裝置會自動選用；
要用別的麥克風就改 `microphone`。

### 4. 開機自動執行

```bat
powershell -ExecutionPolicy Bypass -File install-startup.ps1
```

會在「啟動」資料夾建捷徑，登入後在背景執行（沒有視窗），聽到兩聲上揚的「嗶嗶」就是就緒。
移除：`powershell -ExecutionPolicy Bypass -File install-startup.ps1 -Uninstall`。

## 使用

| 操作 | 結果 |
|---|---|
| 單擊前鍵（`Ctrl+F12`） | 高音嗶：開始錄音 |
| 再單擊前鍵 | 低音嗶：停止，1～2 秒後文字貼到游標處 |
| 錄到 `max_seconds` | 自動停止並送出 |
| `Ctrl+Shift+F12`（鍵盤） | 結束 winvoice |
| 兩聲低音 | 失敗、沒聽到聲音或太短，看記錄檔 |

- 停止前先把游標點在要輸入的欄位。貼上會用剪貼簿，貼完會把原本的文字放回去；辨識出的文字會標記為不進 Windows 剪貼簿記錄（Win+V）與雲端剪貼簿。要完全不碰剪貼簿，把 `output` 設成 `type`（逐字輸入）。
- 辨識還在跑時可以直接開始下一段，結果會依序貼上。
- Stick **長按前鍵會送 Enter**。在病歷系統裡誤觸可能直接送出表單，
  可以在 [設定頁](https://openbrt.github.io/voxstick/config.html)把長按改成「不發送按鍵」
  （設定頁走 WebUSB，這個 fork 在 Windows 上還沒實測過）。
- 微信輸入法也用 `Ctrl+F12`：兩個同時開，winvoice 會搶不到熱鍵，記錄檔會寫明。

## 疑難排解

記錄檔：`%LOCALAPPDATA%\winvoice\winvoice.log`。預設只記秒數和字數，不記內容
（`log_text = true` 才會寫入辨識文字）。

| 記錄檔內容 | 原因 |
|---|---|
| `HTTP 401` | 金鑰錯，或（Azure）金鑰和 `region` 不是同一個資源 |
| `HTTP 403` / `429` | 免費額度用完或請求太頻繁 |
| `network: ...` | 沒網路、防火牆或 Proxy 擋住 `*.speech.microsoft.com` 或 `api.groq.com` |
| `OpenCC unavailable` | 沒裝 `opencc-python-reimplemented`，Groq 的結果不會轉繁體 |
| `dropped a likely Whisper hallucination` | Whisper 在幾乎沒聲音時編出的字幕句，已丟棄 |
| `skipped: ... peak ...` | 太短或幾乎沒聲音：Stick 平放靜音了？麥克風選錯？ |
| `no input device matches` | 找不到 Stick 的麥克風，改用了預設麥克風 |
| `Ctrl+F12 is taken` | 其他程式佔了熱鍵 |
| `SendInput was blocked` | 目標程式以系統管理員身分執行；winvoice 也要用系統管理員執行才能貼進去 |

貼不進某些程式時，把 `output` 改成 `type`（逐字輸入）。

## 隱私

錄音會經 HTTPS 送到雲端辨識，不留在本機：Azure 送到你選的區域，Groq 送到 Groq 的伺服器（美國公司）。
診所使用前，請依院所規範確認病患語音送雲端是否可以接受；各家如何保存、使用語音資料，
請以 Azure 和 Groq 的官方條款為準（這裡沒有替你查證）。

## Wi-Fi（測試中）

Stick 的「Wi-Fi 聽寫」App 會經 Wi-Fi 和 Tailscale 把錄音送到 winvoice，不需要 USB 線，也不用按鍵盤。
計畫與測試步驟見 [`docs/WIFI-PLAN.md`](../../docs/WIFI-PLAN.md)。

- `py winvoice.py --wifi-test`：只收錄音、存成 `wifi-test-<編號>.wav`，不轉寫、不需要金鑰。用來驗證連線。
- 一般執行（`py winvoice.py`）時，winvoice 也會同時等 Stick 連進來；收到的錄音照設定轉寫並貼到游標處。
- `winvoice.ini` 的 `wifi_port`（預設 47100，0 = 關閉）、`wifi_allow`（預設只接受 Tailscale 位址 `100.64.0.0/10`）。
- Windows 防火牆要先允許（系統管理員命令提示字元，一次就好）：
  `netsh advfirewall firewall add rule name="winvoice wifi" dir=in action=allow protocol=TCP localport=47100 remoteip=100.64.0.0/10`

