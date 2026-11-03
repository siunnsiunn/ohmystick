# winvoice：Windows 上用 Stick 聽寫（Azure 語音）

給沒辦法跑本機 Whisper 的 Windows 電腦用，例如診所那台 i3、4 GB 的 Windows 10。
按一下 Stick 前鍵開始錄音，再按一下停止；錄音送到 Azure 語音服務轉成繁體中文，
1～2 秒後貼到游標處。

```
前鍵（Ctrl+F12）─▶ 嗶（高音），開始錄音
前鍵（Ctrl+F12）─▶ 嗶（低音），停止 ─▶ HTTPS 送 Azure ─▶ 文字貼到游標處
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

先用 `short` 加免費方案試；藥名、縮寫常錯，再換 S0 加 `fast` 和詞彙表。
價格以 [Azure 定價頁](https://azure.microsoft.com/pricing/details/cognitive-services/speech-services/) 為準。

## 安裝

### 1. 申請 Azure 語音服務

1. 到 [Azure 入口網站](https://portal.azure.com) 建立「語音服務（Speech service）」資源。
2. 區域選近的，例如 `East Asia`（香港，代號 `eastasia`）；定價層選 `Free F0`
   （一個訂用帳戶只能有一個 F0）。
3. 建好後在「金鑰與端點」複製 **金鑰 1** 和 **位置／區域**。

### 2. 安裝 Python

1. 從 [python.org](https://www.python.org/downloads/windows/) 下載 Windows 64 位元安裝程式（3.12 或 3.13）。
2. 安裝時勾選 **Add python.exe to PATH**，並保留 **py launcher**。
3. 開「命令提示字元」：

```bat
py -m pip install sounddevice
```

### 3. 設定

把 `tools/winvoice` 資料夾複製到電腦上（例如 `C:\winvoice`），然後：

1. 複製 `winvoice.example.ini` 為 `winvoice.ini`，填入 `key` 和 `region`。
2. 用 `fast` 時，複製 `vocab.example.txt` 為 `vocab.txt` 再改成自己的詞。
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

- 停止前先把游標點在要輸入的欄位。貼上會用剪貼簿，貼完會把原本的文字放回去。
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
| `HTTP 401` | 金鑰錯，或金鑰和 `region` 不是同一個資源 |
| `HTTP 403` / `429` | 免費額度用完或請求太頻繁 |
| `network: ...` | 沒網路、防火牆或 Proxy 擋住 `*.speech.microsoft.com` |
| `skipped: ... peak ...` | 太短或幾乎沒聲音：Stick 平放靜音了？麥克風選錯？ |
| `no input device matches` | 找不到 Stick 的麥克風，改用了預設麥克風 |
| `Ctrl+F12 is taken` | 其他程式佔了熱鍵 |
| `SendInput was blocked` | 目標程式以系統管理員身分執行；winvoice 也要用系統管理員執行才能貼進去 |

貼不進某些程式時，把 `output` 改成 `type`（逐字輸入）。

## 隱私

錄音會經 HTTPS 送到你選的 Azure 區域辨識，不留在本機。診所使用前，請依院所規範確認
病患語音送雲端是否可以接受；微軟對語音資料的處理方式請以 Azure 官方文件為準。
