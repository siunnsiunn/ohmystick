# voxstick

> 一根插 Mac 就能用的 **中英语音输入棒**，专门跟**微信输入法**搭配丝滑。
> [English README](README.md)

把麦克风和按键塞进 USB 棒里。**按一下说话，再按一下停，长按一下发送**。
所有事都靠系统自带的输入法（推荐微信输入法）和 USB 标准协议完成
——**不装任何驱动，不装任何 menu bar app**。

```
[USB-C] → 棒 → Mac
         │
         ├ 16 kHz 麦克风 (USB Audio Class)
         └ 蓝牙级别的真键盘 (USB HID)：默认左 Ctrl+F12 / Enter
```

## 真机和屏幕状态

| 真机外观 | 竖放 = 收音 | 屏幕朝上平放 = 闭麦 |
|---|---|---|
| <img src="https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150-stickS3_main-products_02.webp" alt="M5Stack StickS3 真机正面图" width="180"> | <img src="docs/assets/voxstick-upright-live.png" alt="StickS3 竖放，屏幕显示开麦" width="180"> | <img src="docs/assets/voxstick-flat-muted.png" alt="StickS3 平放，LCD 屏幕朝上并显示闭麦" width="180"> |

真机图引用自
[M5Stack StickS3 官方文档](https://docs.m5stack.com/en/core/StickS3)。网页烧录器里已经把 VoxStick 的 LCD 状态画进真机屏幕里，方便用户对刷完后的状态。

## 这跟微信输入法有什么关系

微信输入法 Mac 版有两个**杀手级特性**用作"无线智能听写器"再合适不过：

1. **中英自由混说** —— 程序员说"把这个 PR merge 一下" / "用 React useEffect 做个 hook"，
   它能正确识别中文 + 保留英文术语原拼写，比 macOS 自带 Dictation 强一截。
2. **LLM 驱动的实时纠错** —— 你说话边说边断句、加标点、改错别字，最后把
   整段已经"洗干净"的文本写进光标位置。这个能力在通用 ASR 工具里少见。

但用微信输入法的语音听写有个不爽的地方：**它默认绑 fn 键**，外接键盘
（例如 USB 键盘 / 蓝牙键盘）按 fn 不一定灵。voxstick 解决了这个：

- **轻按棒上的大按键** = 发 `左 Ctrl+F12` HID 报告 → 微信输入法的"免提模式"绑这个键 → 进语音状态
- 边说话边看微信输入法的浮窗实时上字 / 智能纠错
- **再轻按一下** = 第二次发 `左 Ctrl+F12` → 微信输入法停录、把最终文本插进输入框
- **长按棒上的大按键** ≥ 0.6 秒 = 发 `Enter` → 把消息发出去

整个流程**不用碰键盘**。

## 关键卖点

- **手感对**：物理大按键，不是 fn 也不是软件 hotkey。按一下就一下，不会跟系统快捷键打架
- **隐私可控**：内置 6 轴 IMU。把棒**屏幕朝上平放桌面 → 麦克风物理静音**，立起来 → 解禁；平放闭麦可在网页里关闭
- **板载麦克风**：ES8311 codec + 高灵敏 MEMS 麦克风，远离笔记本风扇噪音
- **低功耗状态屏**：240×135 LCD 低亮背光 + 小麦克风图标：
  - 屏幕朝上平放或电脑端 mute = 闭麦图标
  - 立起来 = 开麦图标
  - 说话时 = 底部小音量条跳动
- **跨平台**：UAC + HID 是标准协议，Mac / Windows / Linux 都即插即用。Windows 上甚至更顺
  （Mac 上 F19/F18 这些键被系统/输入法软件名单拦了，Windows 没这些限制）

## 微信输入法配置（关键三步）

1. **设置 → 语音输入 → 免提模式 → 设置快捷键** → 按一下棒上的大按键 → 输入框捕获 `Ctrl+F12`（固件实际发左 Ctrl，macOS 可能显示为 `⌃F12`），保存
2. **系统设置 → 隐私与安全 → 辅助功能** → 打开"微信输入法"开关（首次需要授权）
3. **任意聊天/编辑器** → 把光标点进文本框 → 轻按棒说话 → 再轻按结束 → 长按发送

完事。

## 硬件

[M5Stack StickS3](https://docs.m5stack.com/en/core/StickS3) — 大约 ¥150 / $25，预装一颗 ESP32-S3 + ES8311 codec + 锂电 + LCD + IMU。voxstick 的固件直接刷上去，不用焊接。

## 最快烧录：网页一键安装

普通用户不用装 ESP-IDF，也不用配 Python。直接用网页烧录器：

1. **电脑：**用桌面版 Chrome 或 Microsoft Edge 打开
   <https://openbrt.github.io/voxstick/install.html>
2. **硬件：**用能传数据的 USB-C 线把 M5Stack StickS3 连接到电脑
3. **硬件：**长按 StickS3 侧边 reset/PWR 键约 2 秒，看到机身内部绿色 LED 闪烁后松开，
   这时已经进入 download mode
4. **本页面：**点 **连接并烧录 VoxStick**，在浏览器串口选择框里选择 StickS3 串口并确认安装
5. **硬件：**等烧录完成后拔掉 USB，双击侧边 PWR 键让 StickS3 完全关机，再插回 USB
6. **电脑系统：**固件启动后，在系统声音输入中选择 `StickS3-Mic`
7. **微信输入法：**进入 **设置 → 语音输入 → 免提模式 → 设置快捷键**，按一下棒上的大按键，绑定 `Ctrl+F12`（固件实际发左 Ctrl，macOS 可能显示为 `⌃F12`）
8. **可选配置：**打开 <https://openbrt.github.io/voxstick/config.html>，修改平放闭麦、轻按触发键和长按发送键

这个页面用的是 [ESP Web Tools](https://esphome.github.io/esp-web-tools/)，
会把合并好的 `voxstick-full.bin` 从 `0x0` 一次性写进去。网页安装器源码在
[`docs/install.html`](docs/install.html)，英文页在
[`docs/install-en.html`](docs/install-en.html)，配置页在
[`docs/config.html`](docs/config.html)，manifest 在
[`docs/firmware/v0.1.6/manifest.json`](docs/firmware/v0.1.6/manifest.json)。

如果 GitHub Pages 还没打开，项目维护者需要到仓库
`Settings > Pages > Deploy from a branch`，选择 `main` 分支和 `docs/`
目录。打开后上面的 `openbrt.github.io` 链接就能直接用了。

命令行兜底方式：

```sh
curl -LO https://github.com/openbrt/voxstick/releases/download/v0.1.6/voxstick-full.bin
esptool.py --chip esp32s3 -p /dev/cu.usbmodem* write_flash 0x0 voxstick-full.bin
```

Windows 用户把端口换成类似 `COM5` 即可。

注意：StickS3 带电池，后面还有 M5PM1 PMIC，所以网页烧写结束后不一定能可靠
自动重启。官方按键操作是：长按侧边键进入 download mode，双击关机，单击开机。
烧录完成后用“拔 USB → 双击 PWR 关机 → 再插 USB”最稳。

## 编译 & 烧录

```sh
. $IDF_PATH/export.sh
idf.py build flash
```

ESP-IDF 5.5+。第一次会自动通过 IDF Component Manager 拉取
`espressif/usb_device_uac` / `esp_codec_dev` / `espressif2022/bmi270`。

如果烧坏了进不去 download mode（对 M5StickS3 来说这事儿可能发生），看
[SESSION-NOTES.md](SESSION-NOTES.md) 的 "M5PM1 BOOT-pin brick recovery"
章节。或者跑 `bash tools/trigger-download.sh` 让运行中的固件自己重启进
ROM 下载模式。

## 不只配微信输入法

棒本身只是 USB 标准设备，实际上配任何一个能识别系统麦克风 + 能绑键盘 hotkey
的工具都成：

| Mac 工具 | 触发 | 备注 |
|---|---|---|
| **微信输入法** ⭐ | 左 Ctrl+F12 | 中英混合 + LLM 纠错最强 |
| macOS 原生 Dictation | F19 (重新绑定) | 系统自带，不需安装；中文一般 |
| [VoiceInk](https://github.com/Beingpax/VoiceInk) | F19 | 本地 whisper.cpp，开源免费 |
| [MacWhisper Pro](https://goodsnooze.com/macwhisper) | 自定义 | whisper-large + GPT 纠错，$19 |

如果用 macOS Dictation 或 VoiceInk，可以打开
<https://openbrt.github.io/voxstick/config.html> 把 BtnA 轻按改成 F19 或
F20（更"无冲突"）。微信输入法仍建议保持默认的左 Ctrl+F12。

## 路线图

完成的：
- ✅ 复合 USB 描述符（UAC 麦 + HID 键盘）
- ✅ 物理 PTT 按键 + 长按发送
- ✅ IMU 平放静音
- ✅ LCD 状态显示
- ✅ 网页配置页（平放闭麦、轻按触发键、长按触发键）

可能加的（看反馈）：
- ⏳ 拔 USB 自动深度睡眠（电池续航 24h+）
- ⏳ BLE PTT（拔了 USB 还能键盘 PTT）
- ⏳ Windows 测试 + 更顺的 hotkey 配置（Windows 没 F-key 黑名单）

## License

[MIT](LICENSE)

完整的开发踩坑过程在 [SESSION-NOTES.md](SESSION-NOTES.md)（PMIC L3B LDO 通电、
ES8311 ADC reference 默认配置 bug、M5PM1 BOOT 引脚锁死复位等问题的解决记录）。
