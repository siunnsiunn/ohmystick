# Wi-Fi 測試韌體（階段 T）

`docs/WIFI-PLAN.md` 階段 T 的測試版：原本的六個功能之外，選單第三格多了「Wi-Fi 聽寫」。
韌體裡沒有任何帳號或密碼，Wi-Fi 與 Tailscale 設定都在 Stick 的設定熱點用手機填寫。

由分支 `claude/wifi-plan` 以 ESP-IDF 5.5.1 編譯（`-DVOX_BLE_JUST_WORKS=0`）。

| 檔案 | 位址 | SHA-256 |
|---|---|---|
| `bootloader.bin` | `0x0` | `bb435a553a0137a681fab7da57e41e6b70c4843cc773d03ca173a73207481746` |
| `partition-table.bin` | `0x8000` | `785ee333edb7f2c544ae9c917314cde6bdc2f5189dbda21471c7adb886082f9e` |
| `voxstick.bin` | `0x10000` | `653943419d58a88d540128a9d19e995752072562cd9618031f4192ad74730f23` |

## 刷入（Windows）

1. 安裝 esptool：`py -m pip install esptool`
2. Stick 接上傳輸線，按住電源鍵約 2 秒，綠燈閃爍＝下載模式。
3. 在這個資料夾執行：

```bat
py -m esptool --chip esp32s3 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 voxstick.bin
```

只寫這三段：NVS（藍牙配對、按鍵設定、Wi-Fi 設定）和語音筆記收件匣都不會被清掉。
刷完拔線，按住電源約 6 秒關機，再按兩下電源開機。

之後的步驟見 [`docs/WIFI-PLAN.md`](../../WIFI-PLAN.md) 的「測試步驟」。
