<p align="right">
  <strong>简体中文</strong> · <a href="feishu-handoff.md">English</a>
</p>

# 飞书消息应用接手说明

状态记录于 2026-08-30。当前分支仍在开发，不是可发布版本。

## 工作区与设备

- 仓库：`/Users/bytedance/code/ai-passport`
- 分支：`feature/feishu-messenger-safe`
- ESP-IDF：`/Users/bytedance/esp/esp-idf-v5.5.3`
- 设备串口：`/dev/cu.usbmodem2101`
- 芯片：ESP32-C3，8 MB Flash，无 PSRAM
- 应用分区上限：3 MiB（`0x300000`）
- 当前应用镜像：`build-dev/FoloToy-AI-Passport.bin`，约 2.4 MiB
- 受保护数据：必须保留 `0x356000` 的 `cardid` 和 `0x700000` 的 Recovery。
  迭代时禁止擦除已配网设备，也不要刷完整镜像。
- 移交前已退出串口 monitor，串口可由接手人直接使用。

工作区包含大量未提交的飞书功能导入和原有改动。不要 clean、reset 或覆盖。
当前没有创建 commit，也没有 push。

## 已实现流程

- 手机先单独配置 Wi-Fi；飞书用户自建应用凭据通过另一个 Web Serial 步骤配置。
- OAuth 已绑定并保存在设备中，refresh token 已获得。
- 会话和消息读取使用 user token。
- 飞书原生流式 ASR 采集 16 kHz、16-bit、单声道 PCM，每包 100 ms。
- 文本发送和回复已改用 tenant token，以应用机器人身份发送。
  因当前应用无法获得 `im:message.send_as_user`，该权限已删除。
- 机器人发送修改位于 `main/feishu_api.c`；UI 状态修改位于
  `main/demo_feishu.c`；OAuth 权限位于 `main/feishu_binding.c`。

## 当前故障

用户反馈录音约四秒后卡住。目前没有抓到完整失败日志，因此根因尚未确认。

已有证据：

- 较早一次录音返回飞书 OpenAPI 错误 `10024`（`qps exceeded`），
  `feishu_asr` 在 sequence 1 以 `ESP_ERR_INVALID_RESPONSE` 停止。
- 最新串口日志持续出现证书校验成功，没有 panic、watchdog reset 或重启。
  退出 monitor 时 MCU 仍在运行。
- `feishu_asr_record()` 对每个 100 ms 音频包执行一次同步 HTTP 请求，采集端使用
  4 个缓冲区。应优先检查 `main/feishu_asr.c`、`main/feishu_api.c` 中的
  `feishu_api_asr_stream_packet()`，以及 `main/feishu_http.c` 的 HTTP 传输。
- 后台消息拉取路径曾出现 HTTP 200 但返回 `ESP_ERR_NO_MEM`。此前尝试预分配
  32 KiB 响应缓冲区，反而导致 TLS 分配失败，现已完整回退。不要恢复该实验。

待验证的根因假设：逐包同步 ASR 请求受到限流，或请求阻塞时间过长并填满 4 个
音频缓冲区，使录音界面看起来卡死。修改代码前，应通过带时间戳的 heap、sequence、
HTTP 状态和队列等待日志确认。

## 安全接手步骤

1. 只打开 monitor，不刷机：

   ```bash
   source /Users/bytedance/esp/esp-idf-v5.5.3/export.sh
   idf.py -B build-dev -p /dev/cu.usbmodem2101 monitor
   ```

2. 只录一次短句。不要快速重复尝试，否则可能触发飞书 ASR QPS 限制。
3. 抓取四秒附近最早出现的 `feishu_asr`、`feishu_api`、`feishu_http`、heap、
   panic 或 watchdog 信息。
4. 确认根因后再修改。需要修固件时，应在共享 ASR/API 路径一次修复，并增加最小
   host 回归测试。
5. 只构建并刷应用分区：

   ```bash
   idf.py -B build-dev build
   esptool.py --chip esp32c3 -p /dev/cu.usbmodem2101 write_flash 0x10000 build-dev/FoloToy-AI-Passport.bin
   ```

6. 在机器人已加入的会话中，验证一次录音转写和一次机器人发送。

## 移交时验证状态

```text
Build: PASS（本次录音故障反馈前已完成增量 ESP-IDF 构建）
Host tests: PASS（本接手文档创建前执行过 `./tools/validate.sh --static`）
Device tests: FAIL（当前录音约四秒后卡住）
Unverified: 录音根因与修复、机器人端到端发送、机器人发送补丁后的完整 gate、Recovery 实机启动
```

