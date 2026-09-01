<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport —— 给豆包输入法用的无线麦克风

隔着房间对豆包输入法说话，不需要联网。

这份固件把一张 [FoloToy AI Passport](docs/hardware-design/specifications.zh_CN.md)
卡片变成蓝牙 LE 麦克风。按一下按键、说话，文字就出现在光标所在的位置。卡片采集音频、
推流给 Mac 上的一个小 agent，由它把音频喂给豆包，就像那是一支本地麦克风。

链路里没有 Wi-Fi、没有我们的云服务、也没有手机 —— 这正是它的意义。没网的会议室、
散步途中，只要卡片在笔记本的蓝牙范围内就能用。

```
  卡片                          Mac
  ┌──────────┐   BLE GATT   ┌─────────────┐
  │ 麦克风   │  16 kHz PCM  │ recv-ble    │
  │ 三个按键 ├─────────────►│ agent       │
  │ 屏幕     │  每帧 240    │      ↓      │
  └──────────┘  个采样      │ 虚拟麦克风  │
                            │      ↓      │
                            │ 豆包输入法  │──► 光标处出字
                            └─────────────┘
```

## 按键

| 按键 | 动作 |
| --- | --- |
| 下 | 开始 / 停止录音 |
| 确定 | 发送 —— agent 按下 Enter |
| 上 | 删除上一句（Backspace） |
| 上，长按 | 清空整行 |
| 确定，长按 | 离开语音模式，进入 Wi-Fi / 飞书配置 |

屏幕上是一只杯子，上方有一颗星。你说话时星星会变大、并迸出更多小星 ——
**这就是麦克风真的听到你的证据**，也是区分链路活着还是死了的最省事的办法。屏幕还显示
电量，以及 Claude / Codex 的额度（如果你用这两个）。

## 需要什么

- 一张 FoloToy AI Passport 卡片（ESP32-C3）
- macOS，装好[豆包输入法](https://www.doubao.com/)
- [BlackHole 2ch](https://existential.audio/blackhole/) —— `brew install blackhole-2ch`
- Python 3.9+
- 编译固件需要 [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/)。
  如果直接刷发布好的固件，这一项可以跳过。

暂不支持 Windows 和 Linux：agent 用 macOS 的接口注入按键，也依赖 Core Audio 的虚拟设备。

## 安装

### 1. 刷固件

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py -p /dev/cu.usbmodemXXXX flash
```

不要改 `partitions.csv`。`0x356000` 的 `cardid` 分区和 `0x700000` 的 `recovery`
分区是与厂商小程序安装器约定好的固定契约，挪动它们会破坏 OTA 安装和上键恢复启动。

### 2. 装 Mac 端 agent

```sh
python3 -m pip install bleak sounddevice numpy pyautogui pyobjc-framework-Cocoa
tools/install-mic-agent.sh
```

它会注册一个 launchd agent，登录即启、崩溃自愈。agent 扫描名为 `AI-Passport-Mic`
的设备，卡片休眠或走出范围后会自己重连。

```sh
tail -f /tmp/aipassport-mic.log            # 看它在干什么
launchctl print gui/$(id -u)/com.aipassport.mic | grep state
tools/install-mic-agent.sh --uninstall
```

`bleak`、`sounddevice`、`numpy` 是必需的。缺 `pyautogui` 时 agent 仍能推流，
但按不了键，豆包也就永远不会开始听。`pyobjc` 只是让 agent 不出现在 Dock 里。

### 3. 授权

**系统设置 → 隐私与安全性**：

- **辅助功能** → 添加你的 Python 可执行文件。没有它，macOS 会静默丢弃合成按键，
  于是什么都转不出来。
- **蓝牙** → 添加同一个可执行文件，否则 agent 找不到卡片。

用 `grep -A2 ProgramArguments ~/Library/LaunchAgents/com.aipassport.mic.plist`
查 agent 用的是哪个 Python。

### 4. 让豆包接受虚拟麦克风

**整件事的关键就在这一步，而且它并不显然。**

豆包按 Core Audio 的传输类型筛选麦克风，会拒绝任何报告为 `Virtual` 的设备 ——
而 BlackHole 报告的恰好就是 `Virtual`。所以不管怎么配，BlackHole 都不会出现在
豆包的麦克风列表里。

聚合设备（Aggregate Device）的传输类型报告为 `Unknown`，能通过这个筛选。所以把
BlackHole 包在一个聚合设备里：

1. 打开**音频 MIDI 设置**（在 `/应用程序/实用工具` 里）。
2. 点左下角 **+** → **创建聚合设备**。
3. 在子设备列表里勾选 **BlackHole 2ch**。
4. 起个你认得出的名字，比如 `card`。
5. 在**豆包 → 设置 → 语音**里，把输入选成这个聚合设备。

### 5. 把豆包的语音快捷键设为右 Option

每次录音期间，agent 会一直按住**右 Option** —— 因为豆包只在它的语音键被按住时才
接收音频。把豆包的快捷键设成右 Option，否则音频送到了却没人在听。

### 6. 验证整条链路

按下键，出声数到三。

- 卡片屏幕上的星星应该随你的声音变大。如果没有，问题在麦克风或 BLE 链路，不在豆包。
- 光标处应该在两三百毫秒内开始出字。
- 再按下键停止。

## 不工作的时候

有几种故障看起来像成功。下表按开发过程中实际出现的频率排序。

| 现象 | 可能原因 | 怎么查 |
| --- | --- | --- |
| 出字了，但出的是房间里说的话，不是对卡片说的 | 豆包在听内置麦克风 | 豆包 → 设置 → 语音：选的是聚合设备吗？ |
| 完全不出字，但星星正常变大 | 豆包快捷键不是右 Option，或没给辅助功能权限 | 手动按住右 Option —— 豆包会开始听吗？ |
| 豆包列表里没有 BlackHole | 正常现象 —— 见第 4 步 | 选聚合设备，不是 BlackHole |
| macOS 蓝牙设置里看不到卡片 | 设计如此 —— 它广播为可连接但**不可被发现**，这样附近的 iPhone 无法抢占它唯一的连接 | `tail /tmp/aipassport-mic.log` 应显示 `connected` |
| 停止后豆包还在录 | 语音键卡在按下状态 | 手动按一下再松开右 Option；agent 在静默 20 秒后也会自己松开 |
| 屏幕停在「等待电脑连接」 | agent 没在跑，或缺蓝牙权限 | `launchctl print gui/$(id -u)/com.aipassport.mic \| grep state` |
| 识别不准 | 按正常距离说话，输入增益是按这个距离设的 | `tools/island_agent.py recv-ble --dump /tmp/take.wav` 然后听一遍 |

agent 每次录音结束会打一行关键数字：

```
island: session end 19.4s in=303360 (15658 Hz = 98% of 16k) underrun=23 ... | lost=0f in 0 gaps 0.0%
```

要看的是 `lost=0` —— 它表示 BLE 链路一帧没丢。那个百分比是设备贴合实时的程度，
高于约 90% 在实际使用中听不出差别。

## 原理

空口传的是原始 16 kHz PCM，不是压缩编码。像 ADPCM 这类编码是有状态的，丢一个通知
就会污染之后的全部音频；原始 PCM 只损失那一帧。每帧 240 个采样 —— 15 ms、480 字节
—— 因为 BLE 每个连接事件大约只放行一个通知，而 macOS 实际稳定在约 15 ms 的间隔。
每帧 10 ms 时链路会结构性饿死，只能送达 68% 的实时率。

每帧带一个 16 位序号。BLE 通知不会重传，没有序号的话，「丢了一帧」和「设备只是跑得慢」
无法区分 —— 这个含混不清曾多次把排查引向错误的层。

到了 Mac 上，帧进入一个弹性队列，再排入一条只打开一次、永不关闭的 Core Audio 输出流。
流式识别器容忍延迟但不容忍空洞：塞静音会让它的语音活动检测把句子切成两半，而在两句
之间关闭音频流，看起来就像麦克风被拔掉了。

更多细节：[`docs/software-design/`](docs/software-design/) 是设计文档，
[`docs/hardware-design/specifications.zh_CN.md`](docs/hardware-design/specifications.zh_CN.md)
是硬件规格，[`AGENTS.md`](AGENTS.md) 说明仓库的组织方式。

## 卡片上还有什么

长按确定键后面是一个飞书消息器，有自己的 Wi-Fi 和扫码配置流程：会话列表、消息历史、
语音回复。它早于语音麦克风存在，文档在
[`docs/software-design/feishu-messenger.zh_CN.md`](docs/software-design/feishu-messenger.zh_CN.md)。
它需要 Wi-Fi 和你自己的飞书应用凭据，而这些对语音输入都不是必需的。

## 参与开发

```sh
tools/validate.sh      # 仓库检查、主机测试、固件编译
```

提 PR 之前跑一遍；CI 跑的是同一个脚本。文档默认英文、配一份 `.zh_CN.md`，commit
和 PR 标题用英文 Conventional Commits，界面文案用简体中文。细则在
[`docs/contribution/`](docs/contribution/)。

吉祥物和背景是生成的，不是手绘 —— 改参数后重新生成：

```sh
python3 -m pip install pillow pypng
tools/gen_backdrop.py --preview
tools/gen_mascot.py --preview
```

不要提交任何凭据。固件里不含飞书 App ID 或 Secret；用户自己的凭据在配置阶段通过
USB 写入，保存在 NVS 中。

## 许可

MIT —— 见 [LICENSE](LICENSE)。本项目 fork 自
[FoloToy AI Passport](https://github.com/FoloToy)；上游的硬件指南和 BSP 归其所有，
原始版权声明已保留。
