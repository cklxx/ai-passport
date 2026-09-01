#include "voice_ble.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "voice_ble";

// 128-bit UUIDs (random). Service + audio(notify) + control(notify).
// base 6e40xxxx-b5a3-f393-e0a9-e50e24dcca9e (Nordic-UART-style base).
#define UUID16_SVC   0xFE40
static const ble_uuid128_t UUID_AUDIO = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t UUID_CTRL = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

static uint8_t s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_audio_val_handle;
static uint16_t s_ctrl_val_handle;
static volatile bool s_audio_subscribed;
static volatile bool s_running;
static voice_ble_quota_cb_t s_quota_cb;
static uint16_t s_audio_seq;
static uint16_t s_audio_mtu;
static uint16_t s_audio_msys_min;
static unsigned s_audio_attempts, s_audio_accepted;
static unsigned s_audio_alloc_fail, s_audio_append_fail, s_audio_oversize;
static unsigned s_audio_notify_fail;
static int s_audio_last_rc;

void voice_ble_set_quota_cb(voice_ble_quota_cb_t cb)
{
    s_quota_cb = cb;
}

static int gatt_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    // The PC writes quota packets (island_quota wire format) to the control
    // characteristic; audio/ctrl notifications are handled elsewhere.
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && s_quota_cb != NULL) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[16];
        if (len > 0 && len <= sizeof(buf) &&
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) == 0) {
            s_quota_cb(buf, len);
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &UUID_AUDIO.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_audio_val_handle,
            },
            {
                .uuid = &UUID_CTRL.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_ctrl_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
            // Request the fastest connection interval (7.5 ms) for low audio
            // latency. The central (macOS) may negotiate slower; that's fine.
            struct ble_gap_upd_params up = {
                .itvl_min = 6,   // 6 * 1.25 ms = 7.5 ms
                .itvl_max = 6,
                .latency = 0,
                .supervision_timeout = 200,   // 2 s
            };
            ble_gap_update_params(s_conn, &up);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_audio_subscribed = false;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_audio_val_handle) {
            s_audio_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "audio subscribe=%d", s_audio_subscribed);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (event->conn_update.status == 0 &&
            ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            unsigned interval_x100_ms = desc.conn_itvl * 125;
            ESP_LOGI(TAG, "conn interval=%u (%u.%02ums) latency=%u",
                     desc.conn_itvl, interval_x100_ms / 100,
                     interval_x100_ms % 100,
                     desc.conn_latency);
        } else {
            ESP_LOGW(TAG, "conn update failed rc=%d", event->conn_update.status);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_gap_adv_params adv = { 0 };
    struct ble_hs_adv_fields fields = { 0 };
    const char *name = ble_svc_gap_device_name();

    // Connectable but NON-discoverable: no general/limited discoverable flag.
    // iOS proximity/media-accessory takeover keys off the discoverable flag, so
    // omitting it stops iPhones from popping a "media device" prompt and grabbing
    // the single peripheral connection. The PC agent uses active scanning and
    // still finds us by the advertised name.
    fields.flags = BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    adv.conn_mode = BLE_GAP_CONN_MODE_UND;      // connectable
    adv.disc_mode = BLE_GAP_DISC_MODE_NON;      // not discoverable
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();          // returns only at nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t voice_ble_start(void)
{
    if (s_running) return ESP_OK;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(GATT_SVCS) != 0 ||
        ble_gatts_add_svcs(GATT_SVCS) != 0) {
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set("AI-Passport-Mic");
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
    s_running = true;
    return ESP_OK;
}

void voice_ble_stop(void)
{
    if (!s_running) return;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    s_running = false;
    s_audio_subscribed = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

bool voice_ble_ready(void)
{
    // Connected is enough: we send raw PCM notifications regardless of the CCCD
    // subscribe callback (which some centrals don't surface reliably). If the
    // central hasn't subscribed, ble_gatts_notify_custom simply no-ops — a
    // dropped frame, not a corrupted stream.
    return s_running && s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void voice_ble_reset_audio_seq(void)
{
    s_audio_seq = 0;
}

uint16_t voice_ble_next_audio_seq(void)
{
    return s_audio_seq++;
}

void voice_ble_reset_audio_stats(void)
{
    s_audio_mtu = 0;
    s_audio_msys_min = UINT16_MAX;
    s_audio_attempts = s_audio_accepted = 0;
    s_audio_alloc_fail = s_audio_append_fail = s_audio_oversize = 0;
    s_audio_notify_fail = 0;
    s_audio_last_rc = 0;
}

void voice_ble_log_audio_stats(void)
{
    ESP_LOGI(TAG, "audio: mtu=%u msys_min=%u attempts=%u accepted=%u alloc=%u append=%u oversize=%u notify=%u rc=%d",
             s_audio_mtu, s_audio_msys_min, s_audio_attempts, s_audio_accepted, s_audio_alloc_fail,
             s_audio_append_fail, s_audio_oversize, s_audio_notify_fail,
             s_audio_last_rc);
}

bool voice_ble_send_audio(const uint8_t *data, size_t len, uint16_t seq)
{
    if (!voice_ble_ready() || data == NULL || len == 0) return false;
    s_audio_attempts++;
    s_audio_mtu = ble_att_mtu(s_conn);
    // A notification spends three ATT bytes on opcode + handle. NimBLE otherwise
    // truncates overlong packets and still reports success.
    if (s_audio_mtu < 3 || len + 2 > s_audio_mtu - 3) {
        s_audio_oversize++;
        return false;
    }
    int free = os_msys_num_free();
    if (free >= 0 && free < s_audio_msys_min) s_audio_msys_min = (uint16_t)free;
    uint8_t hdr[2] = { (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8) };
    // The caller retains one rejected frame and retries it before the next capture.
    struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, sizeof(hdr));
    if (om == NULL) {
        s_audio_alloc_fail++;
        return false;
    }
    if (os_mbuf_append(om, data, len) != 0) {
        s_audio_append_fail++;
        os_mbuf_free_chain(om);
        return false;
    }
    int rc = ble_gatts_notify_custom(s_conn, s_audio_val_handle, om);
    if (rc != 0) {
        s_audio_notify_fail++;
        s_audio_last_rc = rc;
    } else {
        s_audio_accepted++;
    }
    return rc == 0;
}

bool voice_ble_send_ctrl(uint8_t code)
{
    if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    // Control codes share the mbuf pool with the audio stream, which holds a block
    // every 15 ms and keeps the pool near empty. A single failed attempt meant the
    // STOP code was simply lost — the device stopped capturing but the PC never
    // learned, so 豆包 kept recording and the stop key felt dead. Audio can afford
    // to lose a frame; control cannot, so retry here. Ten tries over ~50 ms is far
    // below the button's own debounce, and the caller is the worker task, not an
    // ISR, so sleeping is safe.
    struct os_mbuf *om = NULL;
    for (int i = 0; i < 10; i++) {
        om = ble_hs_mbuf_from_flat(&code, 1);
        if (om != NULL) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (om == NULL) return false;
    return ble_gatts_notify_custom(s_conn, s_ctrl_val_handle, om) == 0;
}
