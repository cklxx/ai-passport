<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Stream raw 16 kHz PCM over BLE instead of IMA-ADPCM: ADPCM is a stateful differential codec, so a single dropped BLE notify corrupts the predictor and garbles the rest of the stream. Raw PCM is stateless — a dropped frame costs only 10 ms — and 256 kbps fits BLE's ~700 kbps with room to spare. Removed the ADPCM codec, its host test, and the Python decoder.
- Fixed garbled/3x-speed virtual-mic audio: BlackHole runs at 48 kHz but the stream was opened at 16 kHz, so any app reading it (Doubao IME, QuickTime) heard sped-up noise. `recv-ble` now opens at the device's native rate and upsamples the 16 kHz device audio to match; it also writes every output channel (BlackHole is 2-ch) so stereo/right-channel readers aren't silent.
- Relaxed the device's BLE-ready gate to "connected" rather than "connected and CCCD-subscribed": macOS/CoreBluetooth does not always surface the audio subscribe callback, which left the device stuck on "waiting for PC" even with a live connection. Notifications no-op harmlessly if the central hasn't subscribed.
- Boot straight into the offline BLE voice home instead of the networked Feishu onboarding: the wireless-mic use environment has no Wi-Fi, so requiring a Wi-Fi + Feishu bind before reaching voice deadlocked it. Feishu is now opt-in via an OK long-press.
- Advertise the voice BLE peripheral as connectable-but-non-discoverable: a general-discoverable "AI-Passport-Mic" made nearby iPhones pop a media-accessory prompt and grab the single peripheral connection, which corrupted the Mac's audio stream. The PC agent uses active scanning and still finds it by name; system Bluetooth panels no longer list it (by design).
- Push the Claude quota over the same BLE link: the control characteristic now accepts writes, and `island_agent.py recv-ble` writes the statusline quota packet to it so the island updates without a separate USB-serial/Wi-Fi path. The device decoder is reset on each recording START so takes after the first are not garbled.
- Added a BLE wireless-microphone voice mode as the home screen: the device advertises `AI-Passport-Mic`, captures 16 kHz mono audio, IMA-ADPCM 4:1 compresses it, and streams it over a NimBLE GATT notify service to a paired PC. `tools/island_agent.py recv-ble` decodes the stream into a virtual input device (BlackHole/VB-Cable) so any transcription app reading the system default mic gets the audio; OK toggles capture, DOWN sends (Enter), UP deletes (Backspace). No network dependency — works where there is no Wi-Fi.
- Added a Claude usage island: `tools/island_agent.py statusline` turns the Claude Code merged 7-day rate-limit into a 7-byte packet the device parses (`main/island_quota.h`); the voice screen shows a "Claude 7-day remaining X%" pill (remaining % only — the device has no synced wall clock, so no fabricated countdown).
- Bit-level firmware size optimization: `-Os` with silent assertions, `LV_BUILD_EXAMPLES=n`, and 26 unused LVGL widgets disabled (kept QRCODE + its CANVAS dependency, TJPGD, and the two Montserrat fonts). Removed the Wi-Fi/UDP voice path in favor of BLE.
- Host tests for the new native code: `tests/test_adpcm.c` (sine round-trip error and 4:1 ratio), `tests/test_voice_proto.c` (control-code framing), and `tests/test_island_quota.c` (packet pack/parse), all wired into `tools/validate.sh`; `island_agent.py selftest` cross-checks the Python ADPCM decoder against a C reference vector byte-for-byte.
- Simplified first-run phone setup: the captive portal now accepts Wi-Fi alone and directly serves the iOS captive-network probe; owner Feishu credentials remain a separate second step.
- Report a disabled Feishu refresh-token setting immediately after QR authorization instead of repeatedly replacing an authorization that cannot produce a durable binding.
- Send and reply as the application bot with a tenant token; owner apps no longer request the unavailable `im:message.send_as_user` scope.
- Replaced publisher-built Feishu credentials with advanced owner provisioning: generic Web Serial firmware contains no private app, the same page can write an owner's App ID/Secret locally over USB, and the device then obtains and stores its own user grant.
- Made mini-program BLE install compatibility a template-level invariant: fixed
  protected `cardid`/Recovery partitions, retained the five-second UP-key
  Recovery boot hook, and added CI validation for merged-image structure,
  partition MD5/ranges, the 3 MB app limit, and protected payload exclusion.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Fixed Feishu device-code polling to use the v2 OAuth token endpoint; pending authorization now keeps waiting, transient failures retry, and expired QR codes refresh automatically.
- Replaced the limited LVGL CJK subset with a Flash-resident 1-bit Source Han Sans device font covering U+4E00-U+9FFF and Chinese punctuation, so onboarding and dynamic Feishu message text render without missing-glyph squares.
- Added a direct-device Feishu messenger MVP with encrypted BLUFI provisioning, conversation unread markers, recent-message browsing, selected-message replies, streaming native ASR, review/re-record/cancel actions, and physical send confirmation.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
