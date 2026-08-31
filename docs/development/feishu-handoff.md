<p align="right">
  <a href="feishu-handoff.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Feishu Messenger Handoff

Status captured on 2026-08-30. This branch is active development work, not a
release.

## Workspace and device

- Repository: `/Users/bytedance/code/ai-passport`
- Branch: `feature/feishu-messenger-safe`
- ESP-IDF: `/Users/bytedance/esp/esp-idf-v5.5.3`
- Device port: `/dev/cu.usbmodem2101`
- Target: ESP32-C3, 8 MB Flash, no PSRAM
- Application partition limit: 3 MiB (`0x300000`)
- Current application image: `build-dev/FoloToy-AI-Passport.bin`, about 2.4 MiB
- Protected data: preserve `cardid` at `0x356000` and Recovery at `0x700000`.
  Do not erase the provisioned device or flash a full image during iteration.
- The serial monitor was stopped before handoff, so the port is available.

The worktree contains a large uncommitted Feishu feature import plus existing
changes. Do not clean, reset, or overwrite it. No commit or push was created.

## Implemented flow

- Phone setup stores Wi-Fi first; Feishu owner application credentials are a
  separate Web Serial step.
- OAuth binding is complete and persisted on the device, including a refresh
  token.
- Conversation and message reads use the user token.
- Native Feishu streaming ASR captures 16 kHz, 16-bit mono PCM in 100 ms chunks.
- Text sends and replies now use the tenant token and therefore send as the
  application bot. `im:message.send_as_user` was removed because it is not
  available to this application.
- Bot-send changes are in `main/feishu_api.c`; UI status changes are in
  `main/demo_feishu.c`; OAuth scopes are in `main/feishu_binding.c`.

## Current fault

The user reports that recording hangs after about four seconds. This has not
been reproduced with a complete failure trace, so the root cause is not yet
confirmed.

Known evidence:

- An earlier recording attempt returned Feishu OpenAPI code `10024` (`qps
  exceeded`) and `feishu_asr` stopped at sequence 1 with
  `ESP_ERR_INVALID_RESPONSE`.
- The latest serial output contained recurring certificate-validation messages,
  but no panic, watchdog reset, or reboot. The MCU was still running when the
  monitor was stopped.
- `feishu_asr_record()` performs one synchronous HTTP request for each 100 ms
  audio packet while a four-buffer capture queue feeds it. Inspect this path
  first: `main/feishu_asr.c`, `feishu_api_asr_stream_packet()` in
  `main/feishu_api.c`, and the HTTP transport in `main/feishu_http.c`.
- A separate background message-fetch path has previously reported
  `ESP_ERR_NO_MEM` with HTTP 200. An attempted 32 KiB response preallocation
  made TLS allocation worse and was fully reverted. Do not restore that
  experiment.

Root-cause hypothesis to test: synchronous per-packet ASR requests are either
being rate-limited or blocking long enough to fill the four audio buffers,
which makes the recording UI appear frozen. Confirm this with timestamped heap,
sequence, HTTP-status, and queue-wait logs before changing behavior.

## Safe continuation

1. Start the monitor without flashing:

   ```bash
   source /Users/bytedance/esp/esp-idf-v5.5.3/export.sh
   idf.py -B build-dev -p /dev/cu.usbmodem2101 monitor
   ```

2. Record one short phrase once. Avoid rapid retries because they can trigger
   Feishu ASR QPS limits.
3. Capture the first `feishu_asr`, `feishu_api`, `feishu_http`, heap, panic, or
   watchdog error around the four-second point.
4. Confirm the root cause before editing. If a firmware change is needed, keep
   it in the shared ASR/API path and add the smallest host regression test.
5. Build and flash only the application segment:

   ```bash
   idf.py -B build-dev build
   esptool.py --chip esp32c3 -p /dev/cu.usbmodem2101 write_flash 0x10000 build-dev/FoloToy-AI-Passport.bin
   ```

6. Verify one recording through transcription and one bot send in a chat where
   the bot is present.

## Validation at handoff

```text
Build: PASS (incremental ESP-IDF build before the latest recording report)
Host tests: PASS (`./tools/validate.sh --static` before this handoff document)
Device tests: FAIL (recording currently hangs at about four seconds)
Unverified: recording root cause and fix; end-to-end bot send; complete gate after the bot-send patch; physical Recovery boot
```

