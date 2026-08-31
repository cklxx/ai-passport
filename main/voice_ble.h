#pragma once

// BLE GATT peripheral for voice: advertises "AI-Passport-Mic", exposes one
// service with an audio characteristic (notify, raw 16 kHz PCM frames) and a
// control characteristic (notify + write, one byte). The PC client
// client (tools/island_agent.py recv-ble) subscribes to both.
//
// Threading: init/start/stop are called from the voice worker task. Sending is
// non-blocking — audio frames are dropped if no client is subscribed or the
// controller queue is full, never blocking the capture loop.

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t voice_ble_start(void);
void voice_ble_stop(void);

// True once a central has connected (audio is sent as notifications regardless
// of the CCCD subscribe callback, which some centrals don't surface reliably).
bool voice_ble_ready(void);

// Queue one raw PCM audio frame as a notification. Non-blocking; returns false
// if not ready or the send failed (frame dropped — harmless, PCM is stateless).
bool voice_ble_send_audio(const uint8_t *data, size_t len);

// Notify a control code to the PC (1=send, 2=delete). Non-blocking.
bool voice_ble_send_ctrl(uint8_t code);

// Register a sink for quota packets the PC writes to the control characteristic
// (7-byte island_quota wire format). Called from the BLE host task — the sink
// must be cheap and thread-safe. Pass NULL to clear.
typedef void (*voice_ble_quota_cb_t)(const uint8_t *data, size_t len);
void voice_ble_set_quota_cb(voice_ble_quota_cb_t cb);
