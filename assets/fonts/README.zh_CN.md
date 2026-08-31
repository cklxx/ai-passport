<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 字库资源（Fonts）

本目录存放项目可复用的字库资源。每个字库子目录或单个字库文件，应附说明。

## 如何使用

- 字库文件（如 `.ttf`、`.otf`、LVGL 使用的 C 数组字库等）复制到本目录，并在本项目 `README.md` 记录字名、字号、支持字符集与版权信息。
- 若需集成到 ESP-IDF 固件，参考 [`components/bsp/include/bsp_display.h`](../../components/bsp/include/bsp_display.h) 与 LVGL 字体接口，将字库转换为对应格式并放入正确资源目录。
- 字库占用 Flash 与内存，需在集成前评估 ESP32-C3 无 PSRAM 的限制（详见 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`）。

## 目录说明

## 设备内置字库

`main/lv_font_ai_passport_14.c` 由 Source Han Sans SC Normal 2.005 生成，
采用 14 px、1-bit 压缩字模，覆盖 ASCII、常用中文标点、全角字符以及完整的
U+4E00-U+9FFF 中日韩统一表意文字区。源字库使用 SIL OFL 1.1 许可，许可全文
保存在 `SourceHanSans-OFL.txt`。

重新生成时使用 `lv_font_conv` 1.5.3，字符范围为
`0x20-0x7e,0x2000-0x206f,0x2713,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`，
选择 1 bpp、保留压缩并关闭 kerning。生成后将产品界面的行高和基线分别保持为
17 px 与 4 px。
