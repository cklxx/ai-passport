<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## Unreleased

- PC 侧免手动：`recv-ble` 在设备休眠或走出范围时自动重连而非退出，`tools/install-mic-agent.sh` 把它装成 macOS LaunchAgent，登录即启、崩溃自愈。录音屏改为干净的计时（仅当 BLE 丢帧时提示「信号弱」），不再显示原始调试计数。
- BLE 改传原始 16 kHz PCM，不再用 IMA-ADPCM：ADPCM 是有状态差分编码，BLE notify 丢一帧就会污染 predictor，之后整段音频全乱。原始 PCM 无状态——丢一帧只损失 10 ms——且 256 kbps 远低于 BLE 约 700 kbps 带宽。移除 ADPCM 编解码、主机测试和 Python 解码器。
- 修复虚拟麦克风音频变成杂音/3 倍速：BlackHole 运行在 48 kHz，而流以 16 kHz 打开，任何读取它的应用（豆包、QuickTime）听到的都是加速噪声。`recv-ble` 现在按设备原生采样率打开并把 16 kHz 音频上采样对齐；同时写入每个输出声道（BlackHole 是双声道），避免读右声道/立体声的应用听到静音。
- 放宽设备端 BLE 就绪判断为「已连接」而非「已连接且已订阅 CCCD」：macOS/CoreBluetooth 不总是上报 audio 订阅回调，导致设备连着却卡在「等待电脑」。未订阅时 notify 只是空操作，无害。
- 开机直接进离线 BLE 语音主屏，不再先走联网飞书配网：无线麦克风的使用环境没有 Wi-Fi，原来强制先完成 Wi-Fi + 飞书绑定才能到语音，会永久卡死。飞书改为长按确定键按需进入。
- 语音 BLE 外设改为「可连接但不可被发现」广播：原来通用可发现的 "AI-Passport-Mic" 会让附近 iPhone 弹出媒体外设提示并抢占唯一的从机连接，导致 Mac 音频流被打乱变成纯噪声。PC agent 用主动扫描仍按名字找得到；系统蓝牙面板不再列出它（设计如此）。
- Claude 额度改走同一条 BLE 链路：控制特征现在接受写入，`island_agent.py recv-ble` 把 statusline 额度包写进去更新灵动岛，不再需要单独的 USB-serial/Wi-Fi 通路。每次录音 START 时重置设备端解码器，避免第二次之后的录音串味。
- 新增 BLE 无线麦克风语音模式并设为主屏：设备广播 `AI-Passport-Mic`，采集 16 kHz 单声道音频，经 IMA-ADPCM 4:1 压缩后通过 NimBLE GATT notify 直连推流给已配对 PC。`tools/island_agent.py recv-ble` 将音频解码写入虚拟输入设备（BlackHole/VB-Cable），任何读取系统默认麦克风的转写应用（如豆包输入法）即可拿到音频；确定键开关录音，下键发送（Enter），上键删除（Backspace）。全程不依赖网络，无 Wi-Fi 环境也可用。
- 新增 Claude 用量灵动岛：`tools/island_agent.py statusline` 把 Claude Code 合并后的 7 天限额转成设备可解析的 7 字节数据包（`main/island_quota.h`）；语音屏显示「Claude 7天剩余 X%」药丸（仅剩余百分比——设备无同步时钟，不编造倒计时）。
- bit 级固件体积优化：`-Os` 配合静默断言、`LV_BUILD_EXAMPLES=n`，并关闭 26 个未用 LVGL 控件（保留 QRCODE 及其依赖 CANVAS、TJPGD、两个 Montserrat 字体）。移除 Wi-Fi/UDP 语音链路，改用 BLE。
- 新增原生代码的主机测试：`tests/test_adpcm.c`（正弦往返误差与 4:1 压缩比）、`tests/test_voice_proto.c`（控制码帧格式）、`tests/test_island_quota.c`（数据包打包/解析），全部接入 `tools/validate.sh`；`island_agent.py selftest` 用 C 参考向量逐字节校验 Python 侧 ADPCM 解码器。
- 简化首次手机设置：配网页现在可只保存 Wi-Fi，并直接响应 iOS 热点探测；用户自带飞书应用凭据仍在独立的第二步配置。
- 扫码授权后若飞书应用未允许刷新令牌，设备会直接提示后台配置问题，不再反复生成无法完成长期绑定的二维码。
- 发送和回复改用 tenant token，以应用机器人身份发出；用户应用不再申请不可用的 `im:message.send_as_user` 权限。
- 将发布者内置飞书凭据改为高级用户自带应用：通用 Web Serial 固件不包含私人应用，同一网页可通过 USB 在本地写入用户自己的 App ID/Secret，随后由设备获取并保存自己的用户授权。
- 将小程序 BLE 安装兼容提升为二创模板强制契约：固定保护 `cardid`/Recovery 分区，
  保留上键持续 5 秒进入 Recovery 的 bootloader hook，并在 CI 强制校验合并镜像结构、
  分区表 MD5/范围、3 MB 应用上限和保护分区数据不入包。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 修复飞书 device-code 轮询使用错误 token 地址的问题；未扫码时继续等待，临时网络错误自动重试，二维码过期后自动刷新。
- 将 LVGL 的有限 CJK 子集替换为驻留 Flash 的 1-bit Source Han Sans 设备字库，覆盖 U+4E00-U+9FFF 和中文标点，解决首启引导及飞书动态消息里的缺字方块。
- 新增设备直连飞书消息 MVP：支持加密 BLUFI 配网、会话未读标识、最近消息浏览、回复指定消息、飞书原生流式 ASR、检查/重录/取消，以及按键二次确认发送。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
