<p align="right">
  <a href="voice-link-baseline.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# BLE voice link baseline

This document exists for one reason: **read it before changing any latency or buffer
value on this link.**

Between 2026-09-01 and 09-02 the link was adjusted more than twenty times, several of
those being wrong changes made on wrong evidence and then reverted. Below are the
measured figures, the assumptions that turned out to be false, and why some margins
that look obviously wasteful are not.

## Current baseline (2026-09-02, zero frame loss)

Device side, the `island: device read=` line:

| Field | Healthy | Meaning |
| --- | --- | --- |
| `tried` / `sent` | **equal** | frames encoded / frames the BLE stack accepted |
| `pool_dry` | **0** | times `ble_hs_mbuf_from_flat` returned NULL |
| `refused` | 0 | times the host stack rejected a queued notification |
| `i2s_ovf` | 0-1 | capture-ring overflows, counted in DMA descriptors — NOT BLE frames |

Host side, the `island: session end` line:

| Field | Healthy | Meaning |
| --- | --- | --- |
| realtime rate | **101-107%** | samples received / session duration |
| `lost` | **0f in 0 gaps** | link loss, inferred from the sequence number |
| `underrun` | **0** | starvation DURING the session. Anything above 0 is a defect |
| `tail` | 7-12 | callbacks draining the queue after STOP; expected |
| `lowater` | **30-180ms** | shallowest queue depth reached mid-session |
| `gapmax` | 89-178ms | worst frame arrival gap |
| `stop->release` | **488-503ms** | STOP to push-to-talk release; variance only 15ms |
| `rx1->audible` | 47-103ms | first frame to first audible sample |

## Three hard constraints

**BLE limits notifications per second, not bandwidth.** macOS pins the connection
interval at 30 ms and refuses to negotiate (the firmware's 15 ms request is rejected
with rc=554). Roughly one notification passes per connection event, capping the link
near 34/s. This is the root constraint and it cannot be changed.

**The frame must fit under that cap.** 480 mu-law samples = 30 ms of audio = 482 bytes
= 33.3 notifications/s. The former 240-sample 16-bit PCM frame was also 482 bytes but
needed 66.7/s — a deficit of exactly half, measured at 47-56% delivered. It did not
surface as an error: unsent notifications filled NimBLE's mbuf pool until allocation
returned NULL and **the device dropped the frames itself, before the air**. The host
only saw "the device seems to be running slow".

**The real ceiling is fragments, not the MTU.** A value V occupies V + 3 (ATT) + 4
(L2CAP) bytes, fragmented across 251-byte link-layer PDUs, and the radio passes a
roughly fixed number of *fragments* per connection event. The 252-sample experiment
(506 bytes) cut notifications by 4.8% and raised fragments 43%; delivery fell from 89%
to 69%. A one-parameter fragment model predicts 70% against the measured 69%. So the
oversize guard in `voice_ble.c` tests `V + 7 <= 502`, not the MTU — the MTU alone
admits up to 509, which lands inside the range that reproduces that regression.

## Do not change these, and why

**The prebuffer (`prebuf`, `island_agent.py`).** Nominally 160 ms, effectively
**180 ms**: the queue moves only in whole frames, so `int(0.16 * 16000) = 2560` samples
first trips at the 6th frame, 2880. Measured `lowater` spans the entire depth, 30-180 ms,
with a worst case of 30 ms remaining in a session whose `gapmax` was 178 ms. **The
cushion is fully used; the margin was never generous.**

Two historical reductions failed: 90 ms "measurably broke Doubao's incremental output",
and 100 ms "measured fine on every counter yet lost the first word". On 2026-09-01 it
was cut to 90 ms again, the first word disappeared again, and it was reverted.

**The frame size.** Do not enlarge it to fill the MTU (see the fragment constraint) and
do not shrink it for latency — a 10 ms frame starved the link structurally, delivering
68%.

**The I2S ring depth (`bsp_audio.c`, `dma_desc_num = 10`).** Raising it to 20 broke the
device outright — no key events reached the dispatcher — because these buffers come from
internal RAM and tripling them starved the init path.

## Assumptions that were wrong

Recorded not as penance but because **the same bad evidence is still lying around for
the next person to pick up.**

**"lowater reads 0 every session, so the cushion goes untouched" — the instrument was
dead.** The sample ran unconditionally, and the several callbacks that drain the queue
after STOP all land before the session-end print, so `lowater` read 0 under *every*
condition: the zero-loss sessions and the 16%-loss sessions alike. A number identical
under both measures nothing. Fixed, it says the opposite: the cushion is drawn down to
30-120 ms.

**"underrun is only 8, that's healthy" — it was counting the wrong thing.** All 8 were
the post-STOP drain, not starvation. Split into mid-session `underrun` and `tail`, real
mid-session starvation is **0**.

**"Bandwidth is fine, 256 kbps fits in BLE's 700 kbps" — the arithmetic was right and
the question was wrong.** The question is packets per second. This misframing left
mu-law, the actual fix, on the table for a full day.

**"The reconnect caused it" — 73 paired before/after comparisons put the median cost at
0.0pp.** Reconnects are harmless.

**"Host load / a full disk is the cause" — pearson(load, delivered%) = +0.111**, the
wrong sign. The worst run in the log ran at the lowest load; the best ran at nearly
double it.

**"The 252-sample failure proves packets can't be too full" — it proves fragments can't
multiply.** Same data, the earlier explanation was wrong.

## Rules for changing this link

1. **One parameter at a time, with a control.** Changing three numbers and judging by
   feel is not a measurement.
2. **Green counters do not mean no problem.** It is recorded in this very file: a 100 ms
   prebuffer "measured fine on every counter yet lost the first word". A counters
   argument cannot refute an audible regression.
3. **Verify the instrument is alive before trusting it.** A statistic that reads the same
   when healthy and when broken is worse than none.
4. **The 330 ms inside `stop->release` is Doubao's revision window**, and its evidence
   (250 ms lost the last word, 330 ms did not) was gathered while the link was losing
   12-50% of frames. It may have been compensating for *loss* rather than revision time.
   With loss now zero it is re-testable — but by rule 1.

## A note on NFC

The NTAG213 is a **passive tag with no MCU-facing API**; there is no NFC code in the
firmware and there cannot be. It cannot drive a tap-to-join-Wi-Fi flow: the SoftAP
password is generated randomly per boot by `setup_portal.c` and embedded only in the
on-screen QR code, and the MCU cannot write the tag, which can only hold what was
written at manufacture. A fixed password would be no password at all. (iOS also does not
support joining Wi-Fi from an NFC tag.) The tag's one sensible use is a phone-written
URL pointing at the provisioning page — a one-off configuration, not a firmware feature.
