<p align="right">
  <strong>简体中文</strong> · <a href="feishu-owner-provisioning.md">English</a>
</p>

# 用户自带飞书应用配置

高级模式允许每台设备的所有者使用自己的飞书开发者应用。通用固件、网页资源、发布产物和源码都不包含发布者的 App ID、App Secret、访问令牌或刷新令牌。推荐通过设备的临时安全热点用手机完成一次配置；USB 是备用路径。完成后设备通过 Wi-Fi 直接调用飞书，电脑无需保持在线。

## 1. 最终信任模型

```text
手机本地配置页（推荐）或 USB 配置工具（备用）
  └─ 随机密码临时热点 / 物理 USB
      └─ 用户 App ID 和 App Secret 写入设备 NVS
          └─ 设备显示飞书授权二维码
              └─ 用户访问令牌和刷新令牌写入设备 NVS
                  └─ 设备通过 Wi-Fi 直连飞书
```

设备不会复制或运行 `lark-cli`。高级用户可以让 `lark-cli` 使用同一个飞书应用，但设备只接收该应用的 ID 和 Secret；用户令牌由设备屏幕上的二维码单独授权获得。

## 2. 创建用户自己的飞书应用

由设备所有者本人或所属组织创建飞书开发者应用。把应用可用范围配置为包含目标账号，开启设备授权，并开通固件所需的最小权限：

```text
im:chat:readonly
im:message
im:message.p2p_msg:get_as_user
im:message.group_msg:get_as_user
speech_to_text:speech
offline_access
```

会话读取使用已授权用户的 token；发送和回复使用应用 tenant token，因此消息显示为应用机器人发送，且目标会话必须允许该机器人参与。

还必须在应用安全配置中允许刷新令牌。该开关与 `offline_access` 权限相互独立；未开启时，飞书会接受扫码授权，但只返回短期 access token，设备会主动拒绝这种不完整绑定。

开发者后台开通权限与用户 OAuth 授权是两层独立要求：后台有权限不代表用户已经授权。企业策略还可能要求管理员发布或审批应用。

如果同一个应用也要供 `lark-cli` 使用，可以创建命名配置，并通过标准输入传 Secret，避免它出现在进程列表：

```bash
printf '%s' "$FEISHU_OWNER_APP_SECRET" | \
  lark-cli config init \
    --name ai-passport-owner \
    --app-id cli_example \
    --app-secret-stdin \
    --brand feishu
```

不要把真实 Secret 写入 shell 历史、文档、issue、固件配置或受版本控制的环境文件。

## 3. Web 一键流程

网站继续使用现有 Web Serial 刷机。介绍页必须先提示用户创建企业自建应用、开通所列权限、发布版本、把本人加入可用范围，并从“凭证与基础信息”复制 App ID 和 App Secret。

1. 刷入不含任何飞书凭据的通用完整固件。
2. 手机扫描设备首次配网二维码，在设备本地页面配置家庭 2.4 GHz Wi-Fi。
3. 扫描设备随后显示的二维码，填写用户自己的 App ID 和 App Secret；字段直接通过本地热点写入设备，不上传网站服务器。
4. 已经存在 Wi-Fi、但缺少主人应用的设备会显示带随机 WPA2 密码的 `FoloFeishu-xxxx` 热点二维码；扫码后同样在手机填写。
5. 保存成功后临时热点关闭，设备恢复家庭 Wi-Fi 并显示该用户应用生成的飞书授权二维码。
6. 确认授权后，设备保存自己的用户令牌并进入会话列表；此后不再需要浏览器或电脑。

可嵌入的 USB 备用页面位于 `tools/feishu-provision-web/index.html`。正式部署必须使用 HTTPS，因为浏览器只在安全上下文开放 Web Serial。正式页面不得对凭据字段增加统计、错误上报、持久化或网络提交。

## 4. 本地命令行备用方案

安装 `pyserial`，用 USB 连接设备，并让设备停留在“配置私人飞书”页面：

```bash
python3 -m pip install pyserial
python3 tools/feishu_provision.py \
  --port /dev/cu.usbmodemXXXX \
  --app-id cli_example
```

工具会隐藏 App Secret 输入，不接受把 Secret 直接写在命令行参数中。如果系统只检测到一个 USB 串口，可以省略 `--port`。

## 5. 串口协议

网页和命令行工具共用一行有上限的 UTF-8 协议：

```text
FAP-FEISHU/1 <base64(JSON)>\n
```

解码后的 JSON 只包含：

```json
{"app_id":"cli_example","app_secret":"redacted"}
```

设备回复 `FAP-FEISHU/1 OK` 或 `FAP-FEISHU/1 ERROR`。只有首次引导确认设备没有有效应用配置时才接受该协议。Base64 只负责分帧，不是加密；保密边界依赖物理 USB 连接和可信的本地网页或工具。未增加经过认证的加密会话前，禁止把此配置接收器暴露到 Wi-Fi 或 BLE。

写入另一个应用会先清除旧应用的访问令牌和刷新令牌，再开始新的二维码流程。主机不提供用户 token，因此不需要复制或导出 `lark-cli` 的私人 token 存储。

主人主动写入的记录带有存储版本标记。从旧流程固件升级后，没有该标记的飞书凭据会被拒绝，设备主人需要重新写入一次自己的应用；Wi-Fi 配置不受影响。

## 6. 存储与量产加固

开发版本能够证明流程，但不能保证被拆机读取的 Flash 仍然保密。正式发布前必须：

- 启用 Secure Boot、Flash Encryption 和 NVS Encryption；
- 禁止串口启动日志和崩溃转储输出凭据或 token；
- 只有明确的物理维护/首次引导状态才接受用户应用配置；
- 使用后立即清零输入缓冲；
- 解绑时擦除 App ID、App Secret、访问令牌、刷新令牌和缓存会话状态；
- 明确说明 App Secret 轮换后必须重新灌入；
- 开发设备丢失或可能被物理读取时，撤销飞书授权并轮换该用户的 App Secret。

发布门禁必须扫描源码、生成配置、合并固件和网页打包文件，排查已知的测试或发布者标识。通用固件在应用配置为空时必须等待用户灌入，不能生成发布者应用的二维码。

## 7. 恢复与排障

| 现象 | 含义 | 处理方法 |
| --- | --- | --- |
| 授权页显示别人的应用名 | 镜像或设备里仍有别人的 App ID | 解绑/擦除凭据，刷通用固件，再写入用户自己的应用 |
| 用户没有该应用使用权限 | 应用可用范围或企业审批未包含该账号 | 修改用户自己的应用可用范围并完成发布/审批 |
| Web Serial 无法打开 | 其他标签页、刷机器或串口监视器占用了端口 | 关闭其他传输并重新连接 |
| 设备返回 `ERROR` | App ID/Secret 格式无效或 NVS 写入失败 | 检查 `cli_` App ID 后重试；必要时查看已脱敏设备日志 |
| 扫码成功但设备提示未允许刷新令牌 | 应用安全配置禁止刷新令牌，或缺少 `offline_access` | 允许刷新令牌、开通 `offline_access`、发布应用，再扫描新二维码 |
| App Secret 已轮换 | 设备保存的 Secret 无法再刷新 token | 重新写入用户应用并扫描新二维码 |

设备端解绑只清除本地状态。若要彻底撤销服务端权限，用户还必须在飞书授权管理中取消该应用的授权。
