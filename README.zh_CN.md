<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport —— 给豆包输入法用的无线麦克风

隔着房间对豆包输入法说话，不需要联网。按一下卡片上的键、说话，文字就出现在光标处。

```
  卡片                          Mac
  ┌──────────┐   BLE GATT   ┌─────────────┐
  │ 麦克风   │  16 kHz PCM  │ recv-ble    │
  │ 三个按键 ├─────────────►│ agent       │──► 虚拟麦克风 ──► 豆包 ──► 出字
  │ 屏幕     │  每帧 240    │             │
  └──────────┘  个采样      └─────────────┘
```

## 好用在哪

- **链路里没有网络。** 只有到笔记本的蓝牙 LE。没 Wi-Fi 的会议室照样用。
- **你停它就停。** 静音 3 秒自动结束本次录音。
- **一键发送。** 录音中按确定：停止、等豆包修订完句子、然后回车。
- **屏幕会告诉你它在工作。** 杯子上方的星星随声音长大，活着的链路不会看起来像死的。
- **登录即启、自己恢复。** 卡片休眠或走远后 agent 自动重连，进程崩了 launchd 拉起来。

## 按键

| 按键 | 动作 |
| --- | --- |
| 下 | 开始 / 停止录音 |
| 确定 | 停止并发送 |
| 上 | 删除上一句 |
| 上，长按 | 清空整行 |
| 确定，长按 | 离开语音模式，进入 Wi-Fi / 飞书配置 |

## 安装

```sh
brew install blackhole-2ch python@3.11        # bleak 需要 Python 3.10+
python3.11 -m pip install bleak sounddevice numpy pyautogui pyobjc-framework-Cocoa

. $IDF_PATH/export.sh                          # ESP-IDF v5.5.3
idf.py -p /dev/cu.usbmodemXXXX flash

tools/install-mic-agent.sh                     # launchd agent，登录即启
```

先装依赖再跑脚本：脚本自己不装任何东西，只是去找一个已经装好的解释器。而且它只检查
`bleak`、`sounddevice`、`numpy` 三个 —— 漏掉 `pyautogui` 的话，agent 会欢快地推流，
但一个字都不会打出来。

然后是三个容易漏掉的设置：

1. **系统设置 → 隐私与安全性 → 辅助功能 和 蓝牙**：把 agent 用的那个 Python 加进去。
   没有辅助功能权限，macOS 会静默丢弃合成按键，于是什么都转不出来。
2. **音频 MIDI 设置 → + → 创建聚合设备**，勾选 **BlackHole 2ch**，起个名字，然后在
   豆包里把麦克风选成*它*。豆包会拒绝 Core Audio 传输类型为 `Virtual` 的设备，而
   BlackHole 报告的正是 `Virtual`；聚合设备报告 `Unknown`，能通过。
   **BlackHole 自己永远不会出现在豆包的列表里 —— 整件事的关键就在这一步。**
3. **豆包的语音快捷键必须设为右 Option**，因为 agent 在每次录音期间一直按住这个键。

验证：按下键，出声数到三。星星应该变大，文字应该出现。
`tail -f /tmp/aipassport-mic.log` 能看到 agent 在做什么，
[设计文档](docs/software-design/)里写了那些「看起来像成功」的故障 ——
头号是豆包悄悄在听内置麦克风。

## 原理

**传原始 PCM，不用编码。** ADPCM 是有状态的，丢一个通知就污染之后的全部音频。原始
16 kHz PCM 只损失那一帧，而 256 kbps 在 BLE 的预算里还很宽裕。

**帧长按连接间隔来定。** BLE 每个连接事件大约只放行一个通知，而 macOS 稳定在约
15 ms。所以一帧是 240 个采样 —— 15 ms、480 字节，ATT MTU 512 内最大的整数 PCM 帧。
每帧 10 ms 时链路结构性饿死，只送达 68% 的实时率；15 ms 时是 92-100%。

**每帧带一个序号。** BLE 通知不会重传，没有序号的话，「丢了一帧」和「设备只是跑得慢」
无法区分。这个含混把排查引向错误的层三次；加上计数器后一个会话就定了案 ——
丢失发生在固件内部，不在空口。

**用背压，不用队列。** 采集 worker 只留一个重试位，不做缓冲。曾试过 4 帧队列又撤掉了：
它只能按链路的速度排空，而采集一直在填，结果 92% 的循环迭代发现队列满、什么都没发出去。

**输出流永不关闭。** 流式识别器容忍延迟但不容忍空洞。塞静音会让它的语音活动检测把
句子切成两半，而在两句之间关闭音频流，看起来就像麦克风被拔掉 —— 所以流只打开一次，
用 100 ms 预缓冲吸收 BLE 抖动。

**控制码的优先级高于音频。** 它们和每 15 ms 取一块的音频流共用 BLE 的 mbuf 池。
丢一帧音频是 15 ms 的声音；丢一个 STOP 会让豆包永远录下去。所以控制码会重试，
而且只在真正发出后才清除。

**信号在设备上做过处理。** 输入增益是 24 dB 而不是 30 —— 30 dB 下正常说话就会削顶，
而削顶是不可恢复的失真，识别器对它退化很快。一个单极 90 Hz 高通滤掉手持噪声和体传
震动，那些占了 11% 的能量，而 300-3400 Hz 的语音带只占 36%。

## 卡片上还有什么

长按确定键后面是一个飞书消息器，有自己的 Wi-Fi 和扫码配置。它早于麦克风存在，需要
你自己的飞书应用凭据，对语音输入不是必需的。见
[`docs/software-design/feishu-messenger.zh_CN.md`](docs/software-design/feishu-messenger.zh_CN.md)。

## 参与开发

```sh
tools/validate.sh      # 仓库检查、主机测试、固件编译
```

CI 跑同一个脚本。文档默认英文、配一份 `.zh_CN.md`；commit 用英文 Conventional
Commits；界面文案用简体中文。细则在 [`docs/contribution/`](docs/contribution/)。

不要改 `partitions.csv` —— `0x356000` 的 `cardid` 和 `0x700000` 的 `recovery`
是与厂商安装器约定的固定契约。吉祥物和背景是生成的：
`tools/gen_mascot.py --preview`、`tools/gen_backdrop.py --preview`。

## 许可

MIT —— 见 [LICENSE](LICENSE)。fork 自
[FoloToy AI Passport](https://github.com/FoloToy)；上游的硬件指南和 BSP 归其所有，
原始版权声明已保留。
