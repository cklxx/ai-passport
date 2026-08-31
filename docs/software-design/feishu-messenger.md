<p align="right">
  <a href="feishu-messenger.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Feishu Messenger Product and Technical Specification

This document is the implementation baseline for the ESP32-C3 AI Passport Feishu messenger. The device calls Feishu OpenAPI directly and requires no phone, computer, or cloud bridge at runtime. It does not run the `lark-cli` executable; firmware implements the equivalent authentication and messaging APIs required by this product.

## 1. Product scope

The first accepted release provides only four core capabilities:

1. Show recent conversations and a dot when a message arrived after this device last viewed that conversation.
2. Show recent messages in the selected conversation.
3. Select a specific message as the reply target.
4. Record speech, transcribe it, and reply to that message after confirmation.

The first release does not render images, play voice messages, expose thread details, edit sent messages, or promise the same exact unread count as the Feishu client. Image, file, audio, and other message types use textual placeholders.

## 2. First use and every boot

The device does not show the development demo menu or require the user to find `Setup`. Startup routing is fixed:

```text
boot
 ├─ no usable Wi-Fi → AP provisioning page
 ├─ Wi-Fi available but no Feishu account → Feishu binding QR
 └─ Wi-Fi and Feishu account available → conversation list
```

### 2.1 AP provisioning

- The device advertises `FoloPassport-XXXX`. The screen shows a hotspot QR, SSID, and `192.168.4.1` fallback address.
- The phone joins the hotspot and opens a Chinese setup page to select 2.4 GHz Wi-Fi and enter its password.
- After submission, the device keeps the AP available while attempting the station connection. Success closes the AP and advances to Feishu binding. Failure keeps the setup page available with a specific reason and permits another submission.
- If saved Wi-Fi cannot connect within 15 seconds at boot, provisioning starts automatically without requiring a restart.
- Wi-Fi passwords never appear on screen or in logs.

### 2.2 Feishu account binding

- Generic firmware contains no Feishu App ID/Secret. An advanced owner writes their own developer application's credentials locally over physical USB or Web Serial before authorization.
- The device then displays a device-authorization QR for that owner-provided application. The intended user must be included in that application's availability and grant the requested permissions; tenant administrators can still restrict application access or permissions.
- The screen shows the device authorization QR and remaining lifetime. If it is not scanned, expires, or a transient service error occurs, the device replaces it automatically instead of entering a permanent failure page.
- Once the device has safely stored both access and refresh tokens, it shows binding success and automatically opens conversations. A failure to load profile or conversations must not turn a successful binding into a binding failure.
- After restart, cached tokens are preferred. Rebinding is required only after Feishu explicitly rejects the user token and also rejects refresh. DNS, TLS, timeout, or offline errors must not clear binding.

## 3. Pages, navigation, and copy

The normal path has four pages. Loading, offline, empty, and failure states render within the current page instead of creating more intermediate pages.

| Page | Primary content | `UP` / `DOWN` | `OK` click | Hold `OK` |
| --- | --- | --- | --- | --- |
| Conversations | name, latest summary, new-message dot | move selection | open conversation | open settings |
| Messages | sender, time, summary; selected row expands to three lines | move selection | reply to selected message and start recording | back to conversations |
| Recording | reply target, duration, level indicator | none | stop and transcribe | cancel to messages |
| Review | complete recognized text | `UP` record again, `DOWN` cancel | send reply | cancel to messages |

Settings is a low-frequency maintenance entry and is not part of the normal path. It contains only re-provision Wi-Fi and unbind Feishu; unbinding requires a second confirmation.

The visual design uses one flat list and whitespace, not grids of cards. All user-facing copy uses the bundled CJK font. Conversation names, sender names, and messages are truncated only at UTF-8 boundaries, never as missing glyph boxes or partial characters. A fixed footer shows available keys in Chinese without abbreviations.

## 4. Conversation, message, and unread rules

- A user token lists visible P2P and group chats, retrieves recent messages, and replies to the selected `message_id` as the user.
- The first sync after binding records each conversation's current newest message as its baseline and does not mark history as new.
- The device polls every 30 seconds. A new incoming message newer than the local read baseline adds a dot. The first release guarantees only new/not-new, not an exact count.
- The read baseline advances only after a conversation opens and its messages render successfully. Merely selecting a conversation does not clear the dot.
- Keep the latest 8 conversations and 12 messages. Preserve selection across refreshes when possible; otherwise select the nearest remaining row.
- Empty conversations show `No messages`. A failed refresh preserves cached content and shows an inline retry state instead of clearing the list.

Replies use a stable idempotency ID. Definite failures may be retried. If a request times out with an unknown result, refresh messages before allowing another send; never retry automatically and risk a duplicate.

## 5. Speech-to-text reply

- Capture 16 kHz, 16-bit mono PCM and stream it in small blocks. Never allocate a whole-recording buffer.
- Obtain the tenant token lazily when recording starts; it must not block boot or the conversation list.
- `OK` stops recording. Silence timeout or the 30-second cap also stops it. Empty recognition returns to recording with a clear retry prompt.
- Successful recognition opens the single review page. Send, re-record, and cancel all happen there; there is no separate actions page.
- ASR failure preserves the reply target and allows retry. Cancel closes audio and network streams without leaving a worker task behind.

## 6. Software states and recovery

Startup and interaction use explicit state machines:

```text
BOOT → WIFI_CHECK → AP_PROVISION → WIFI_CONNECT
                    ↓ success
       APP_CHECK → USB_PROVISION → QR_BIND → INITIAL_SYNC → CHATS

CHATS ⇄ MESSAGES → RECORDING → TRANSCRIBING → REVIEW → SEND → MESSAGES
```

All network requests use one authentication-aware API client:

- HTTP 2xx and successful business code: commit the result.
- Definite user-token expiry: refresh once, then replay the original request once.
- Explicit refresh-token rejection: preserve Wi-Fi and enter QR binding.
- DNS, TLS, offline, or server 5xx: back off while preserving page, cache, and binding.
- QR authorization pending is normal, not an error; expiry generates a new QR automatically.
- Out of memory releases request resources and returns to an operable page; it never erases NVS or enters a reboot loop.

Logs contain only state, HTTP status, Feishu business error code, free heap, and largest contiguous heap. They must not contain QR parameters, App Secret, access/refresh tokens, Wi-Fi passwords, or message bodies.

## 7. Authentication, storage, and memory boundaries

- Firmware and release images contain no Feishu App ID, App Secret, or user token. An advanced user provisions their own Feishu application over the physical USB Serial/JTAG link after flashing. The device then performs device authorization for that application and stores the resulting user tokens locally.
- The versioned USB protocol is accepted only while onboarding has no application credentials. Importing another application clears access and refresh tokens from the previous application. App Secret and tokens must never appear in logs or a generated firmware image.
- Access tokens can exceed 7 KiB. TLS completes its handshake before firmware writes the long `Authorization` header in chunks, preventing simultaneous copies of the token and large HTTP/TLS buffers.
- User tokens serve chat, message, and reply APIs. A tenant token serves ASR only and is fetched on demand. It is not prefetched during startup.
- JSON responses stream into bounded storage and are parsed after TLS/HTTP objects are released. UI models have fixed capacities and UTF-8-safe truncation.
- ESP32-C3 has no PSRAM. Even under the heaviest TLS plus Wi-Fi path, the target is at least 20 KiB free heap and a 12 KiB largest contiguous block. Log and validate both around every critical request.
- Production firmware must enable Flash Encryption and NVS Encryption. Development firmware must still avoid serial disclosure of secrets.

## 8. Implementation order and acceptance gates

Implementation proceeds in this order, and firmware is flashed only after the previous layer passes:

1. Pure C state machines: startup routing, forward/back navigation, QR renewal, auth error classification, first-sync unread baseline, and send idempotency.
2. API/parser fixtures: long tokens, pagination, empty chats, non-text messages, 401 refresh, 5xx, and truncated JSON.
3. Static checks and a complete firmware build.
4. One end-to-end device acceptance run.

The device matrix covers: factory-fresh AP provisioning, incorrect Wi-Fi retry, automatic QR renewal without scanning, automatic advance after scanning, power-cycle without another scan, offline recovery, rebind after token revocation, no false initial unread state, new-message dot, voice reply to a specific message, empty recognition/re-record/cancel, exactly-once send behavior, and 20 minutes of repeated use without reboot or sustained heap loss.

Delivery reports Build, Host tests, Device tests, and remaining unverified items separately. A successful compile is not hardware acceptance.
