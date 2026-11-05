# Wi-Fi 區網韌體（階段 L：驗證＋加密）

`docs/WIFI-PLAN.md` 階段 T 不通過後的備案：Stick 不經 Tailscale，直接用診所區網連到 CLINIC 的 winvoice。
由分支 `claude/wifi-lan` 以 ESP-IDF 5.5.1 編譯。韌體裡沒有任何帳號或密碼。

**安全性**：連線建立時，兩邊用「配對密碼」互相證明身分（HMAC-SHA256，雙方各出一個隨機數），
之後每一筆資料都用 AES-256-GCM 加密並驗證，方向各一把金鑰。不知道配對密碼的裝置連不進來，
也看不到或改不了錄音。**沒設配對密碼（或少於 12 個字元），Stick 和 winvoice 都不會連線。**
實機測試還沒做過，在那之前請只說測試用的句子。

| 檔案 | 位址 | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `01ff8d7066155330d2b38645cd7690d79b6081896afde4b9d7160ae7a9c1ec90` |
| `partition-table.bin` | `0x8000` | `5295e8c42d6fb5f95e4e8a185932c6191d27cfc5078322475347e40e71af815a` |
| `voxstick.bin` | `0x10000` | `8d9598a34d9ba91f4f14bdd1c219d6eb50da9c5c9012516831fbce01352cc796` |

## 刷入（Windows）

1. 先結束 winvoice（`Ctrl+Shift+F12`）。
2. Stick 接上傳輸線，按住電源鍵約 2 秒，綠燈閃爍＝下載模式。
3. 在這個資料夾執行：

```bat
py -m esptool --chip esp32s3 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 voxstick.bin
```

只寫這三段，NVS（Wi-Fi 設定、藍牙配對）和語音筆記收件匣都不會被清掉。
刷完拔線，按住電源約 6 秒關機，再按兩下電源開機。

## 設定與測試

1. **電腦端裝套件**：`py -m pip install cryptography`
2. **產生配對密碼**（會印出 20 個英數字，抄下來，等一下兩邊都要填）：
   `py -c "import secrets;print(secrets.token_hex(10))"`
3. **`winvoice.ini`**：在 `[winvoice]` 底下加一行 `wifi_key = <剛才那 20 個字>`。
   `wifi_allow` 預設已經包含 `192.168.0.0/16`，不用改（想更嚴可以改成 `192.168.1.0/24`）。全檔每個設定只能有一行。
4. **防火牆**（系統管理員命令提示字元，一次就好）：
   `netsh advfirewall firewall add rule name="winvoice lan" dir=in action=allow protocol=TCP localport=47100 remoteip=192.168.1.0/24`
5. 在 `C:\winvoice` 執行 `py winvoice.py --wifi-test`，先放著。
6. **Stick 設定**：選單進第三格「Wi-Fi 聽寫」，因為還沒有配對密碼，會進設定模式，畫面顯示 `OHMYSTICK-xxxx` 和 8 位數密碼。
   手機連上，瀏覽 `http://192.168.4.1`：
   - Wi-Fi 名稱、密碼：診所 Wi-Fi（密碼留空 = 沿用已存的）
   - 配對密碼：第 2 步那 20 個字
   - 電腦的區網位址：`192.168.1.199`
   - 埠號：`47100`
   按「儲存並重新開機」。Stage T 留下的 Tailscale auth key 會在這時從 Stick 裡刪掉。
7. 重開後進「Wi-Fi 聽寫」。畫面依序：連線中（Wi-Fi）→ 未連線（找電腦）→ 可錄音，下方顯示 `LAN AES`。
   - 如果停在「未連線」且下方出現 `KEY MISMATCH`：Stick 和電腦的配對密碼不一樣，回設定頁（長按前鍵 3 秒）重填。
8. 按住前鍵說一句測試用的話、放開；電腦印出秒數、幀數、遺失數並存成 `wifi-test-<編號>.wav`，Stick 顯示「完成」。

## 回報

從進入畫面到「可錄音」花幾秒、`--wifi-test` 印出的遺失數、WAV 聽起來是否正常、放 30 分鐘有沒有斷線與電量變化。
電腦端如果印出 `refused ... did not prove it knows wifi_key`，代表密碼不符；`failed authentication` 代表資料被改過或版本不合。

## 平常聽寫

winvoice 一般模式（`py winvoice.py`）在有 `wifi_key` 時才會開網路接收；沒填就只用 USB。
