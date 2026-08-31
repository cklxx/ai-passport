<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy AI Passport

# AI Passport Feishu Messenger

This repository turns the FoloToy AI Passport into a standalone Feishu
messenger. The firmware runs directly on the ESP32-C3 device; it does not
depend on a phone, desktop relay, or bridge service after setup. The complete
product code is maintained directly on `main`.

## Current features

- AP and BLUFI network provisioning
- QR-code Feishu user authorization
- Conversation list with unread indicators
- Message history and individual message details
- Direct voice-to-text messages and replies
- Feishu image download and on-device preview
- Chinese UI font and numeric battery percentage
- Periodic foreground and background message refresh

See the [English design document](docs/software-design/feishu-messenger.md) or
the [Simplified Chinese design document](docs/software-design/feishu-messenger.zh_CN.md)
for the product flow, architecture, permissions, and known limits.

## Build and test

The target is ESP32-C3 with 8 MB flash and ESP-IDF 5.5.3.

```bash
source "$IDF_PATH/export.sh"
./tools/validate.sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Configure the Feishu application ID and secret through ESP-IDF project
configuration. Never commit production credentials. Production devices should
enable Flash Encryption and NVS Encryption.

This project is based on the open-source
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) hardware and BSP
baseline. Its original copyright notices and MIT License are retained.

## Contributing

Issues, discussions, documentation improvements, tests, and pull requests are
welcome. Please read [CONTRIBUTING.md](.github/CONTRIBUTING.md) before sending a
change. The project is available under the [MIT License](LICENSE).
