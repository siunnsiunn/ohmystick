# Wi-Fi 區網測試韌體（階段 L1）

`docs/WIFI-PLAN.md` 階段 T 不通過後的備案：Stick 不經 Tailscale，直接用診所區網連到 CLINIC 的 winvoice。
由分支 `claude/wifi-lan` 以 ESP-IDF 5.5.1 編譯。韌體裡沒有任何帳號或密碼。

> **這一版的連線沒有加密。** 只用來確認「區網直連」走得通，**請只說測試用的句子，不要錄病患的語音**。
> 加密（階段 L2）完成前，不要拿它做正式聽寫。

| 檔案 | 位址 | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `01ff8d7066155330d2b38645cd7690d79b6081896afde4b9d7160ae7a9c1ec90` |
| `partition-table.bin` | `0x8000` | `5295e8c42d6fb5f95e4e8a185932c6191d27cfc5078322475347e40e71af815a` |
| `voxstick.bin` | `0x10000` | `20b872d2ac714f63067fc09c5666b62d8c4bcf402ef7b995e9beb80be7bad2ff` |

## 刷入（Windows）

1. 先結束 winvoice（`Ctrl+Shift+F12`）。
2. Stick 接上傳輸線，按住電源鍵約 2 秒，綠燈閃爍＝下載模式。
3. 在這個資料夾執行：

```bat
py -m esptool --chip esp32s3 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 voxstick.bin
```

只寫這三段，NVS（Wi-Fi 設定、藍牙配對）和語音筆記收件匣都不會被清掉。
刷完拔線，按住電源約 6 秒關機，再按兩下電源開機。

## 測試

1. **防火牆**（系統管理員命令提示字元，一次就好；先刪掉舊的 Tailscale 規則也可以）：
   `netsh advfirewall firewall add rule name="winvoice lan" dir=in action=allow protocol=TCP localport=47100 remoteip=192.168.1.0/24`
2. **winvoice.ini** 的 `[winvoice]` 底下，把 `wifi_allow` 改成 `192.168.1.0/24`（全檔只能有一行 `wifi_allow`，改原本那行）。
3. 在 `C:\winvoice` 執行 `py winvoice.py --wifi-test`，先放著。
4. Stick 選單進第三格「Wi-Fi 聽寫」。上次的設定還在，會直接嘗試連線。**要改位址：** 長按前鍵 3 秒進設定模式，
   手機連 `OHMYSTICK-xxxx` 熱點、瀏覽 `http://192.168.4.1`，
   - Wi-Fi 名稱、密碼：診所 Wi-Fi（密碼留空 = 沿用已存的）
   - 配對密碼：**留空**（L2 才用）
   - 電腦的區網位址：`192.168.1.199`
   - 埠號：`47100`
5. 畫面應依序顯示 連線中（Wi-Fi）→ 未連線（找電腦）→ 可錄音，下方顯示 `LAN`。
6. 按住前鍵說一句測試用的話、放開；電腦會印出秒數、幀數、遺失數並存成 `wifi-test-<編號>.wav`，Stick 顯示「完成」。

## 回報

從進入畫面到「可錄音」花幾秒、`--wifi-test` 印出的遺失數、WAV 聽起來是否正常、放 30 分鐘有沒有斷線與電量變化。
