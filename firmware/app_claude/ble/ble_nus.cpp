/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * NimBLE-based NUS server. Single connection, peripheral role.
 */
#include "ble_nus.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char* TAG = "ble_nus";

// ─── UUIDs (Nordic UART Service) ─────────────────────────────────────
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

// ─── State ───────────────────────────────────────────────────────────
static uint16_t s_conn_handle = 0xffff;     // 0xffff = no connection
static uint16_t s_tx_attr_handle = 0;
static bool     s_tx_subscribed = false;
static uint32_t s_current_passkey = 0;

static ble_nus_rx_line_cb s_rx_cb = nullptr;
static void*              s_rx_user = nullptr;

// Line accumulator for inbound writes. NUS is a stream, daemon may
// fragment a single JSON line across multiple writes.
static char     s_rx_buf[1024];
static size_t   s_rx_len = 0;

// ─── Helpers ─────────────────────────────────────────────────────────

static void rx_feed(const uint8_t* data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_rx_len > 0) {
                s_rx_buf[s_rx_len] = 0;
                if (s_rx_cb) s_rx_cb(s_rx_buf, s_rx_len, s_rx_user);
                s_rx_len = 0;
            }
        } else if (s_rx_len < sizeof(s_rx_buf) - 1) {
            s_rx_buf[s_rx_len++] = c;
        }
    }
}

// ─── GATT service definition ─────────────────────────────────────────

static int gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg);
static int gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg);

// NimBLE struct ble_gatt_chr_def field order:
//   uuid, access_cb, arg, descriptors, flags, min_key_size, val_handle, cpfd
// C++20 requires designated initializers to match declaration order, so
// we list fields in order rather than relying on designators.
static const struct ble_gatt_chr_def s_chr_defs[] = {
    {
        /* uuid          */ &NUS_RX_UUID.u,
        /* access_cb     */ gatt_rx_access,
        /* arg           */ nullptr,
        /* descriptors   */ nullptr,
        /* flags         */ BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        /* min_key_size  */ 0,
        /* val_handle    */ nullptr,
        /* cpfd          */ nullptr,
    },
    {
        /* uuid          */ &NUS_TX_UUID.u,
        /* access_cb     */ gatt_tx_access,
        /* arg           */ nullptr,
        /* descriptors   */ nullptr,
        /* flags         */ BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
        /* min_key_size  */ 0,
        /* val_handle    */ &s_tx_attr_handle,
        /* cpfd          */ nullptr,
    },
    {  // terminator (uuid==nullptr)
        nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr,
    },
};

// NimBLE struct ble_gatt_svc_def field order:
//   type, uuid, includes, characteristics
static const struct ble_gatt_svc_def s_svc_defs[] = {
    {
        /* type            */ BLE_GATT_SVC_TYPE_PRIMARY,
        /* uuid            */ &NUS_SVC_UUID.u,
        /* includes        */ nullptr,
        /* characteristics */ s_chr_defs,
    },
    {  // terminator (type==0)
        0, nullptr, nullptr, nullptr,
    },
};

static int gatt_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const struct os_mbuf* om = ctxt->om;
        // Drain the mbuf chain.
        while (om != nullptr) {
            rx_feed(om->om_data, om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* /*arg*/)
{
    // TX is notify-only from our side; reads return empty.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// ─── Advertising ─────────────────────────────────────────────────────

static int gap_event_handler(struct ble_gap_event* event, void* arg);

static void start_advertising(void)
{
    // 31-byte advertising packet split:
    //   - main advertising: flags (3B) + 128-bit UUID (18B)  = 21B
    //   - scan response   : complete device name (≤29B for 31B max)
    // The Mac daemon scans by NUS UUID first, name second, so this
    // arrangement is fine and avoids EMSGSIZE.
    struct ble_hs_adv_fields adv = {};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t*)&NUS_SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {};
    const char* name = ble_svc_gap_device_name();
    rsp.name = (uint8_t*)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
        // Non-fatal: continue advertising without scan response.
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", name);
}

static int gap_event_handler(struct ble_gap_event* event, void* /*arg*/)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected; handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason=%d", event->disconnect.reason);
        s_conn_handle = 0xffff;
        s_tx_subscribed = false;
        s_current_passkey = 0;
        s_rx_len = 0;
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_attr_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "TX subscribed=%d", (int)s_tx_subscribed);
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated; conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Drop the old bond and accept the new pairing.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // Random 6-digit passkey, displayed by the watch.
            uint32_t pk;
            extern int os_get_rand_bytes(uint8_t* buf, uint8_t len);
            do {
                ble_hs_hci_util_rand(&pk, sizeof(pk));
                pk %= 1000000;
            } while (pk < 100000);
            s_current_passkey = pk;

            struct ble_sm_io io = {};
            io.action = BLE_SM_IOACT_DISP;
            io.passkey = pk;
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "passkey display: %06u (inject rc=%d)", (unsigned)pk, rc);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change; status=%d", event->enc_change.status);
        s_current_passkey = 0;
        break;

    default:
        break;
    }
    return 0;
}

// ─── Host stack init plumbing ────────────────────────────────────────

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset; reason=%d", reason);
}

static void host_task(void* /*param*/)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ─── Public API ──────────────────────────────────────────────────────
// All public functions live inside extern "C" so the symbols match what
// ble_nus.h declares. Without this, name mangling makes them invisible
// to callers who include the header.

extern "C" {

void ble_nus_set_rx_callback(ble_nus_rx_line_cb cb, void* user)
{
    s_rx_cb = cb;
    s_rx_user = user;
}

bool ble_nus_is_connected(void)
{
    return s_conn_handle != 0xffff && s_tx_subscribed;
}

uint32_t ble_nus_current_passkey(void) { return s_current_passkey; }

int ble_nus_send(const char* json, size_t len)
{
    if (s_conn_handle == 0xffff || !s_tx_subscribed) return -1;

    // Append newline if caller didn't.
    static char tmp[512];
    if (len + 2 > sizeof(tmp)) return -2;
    memcpy(tmp, json, len);
    if (len == 0 || tmp[len - 1] != '\n') {
        tmp[len++] = '\n';
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(tmp, len);
    if (!om) return -3;
    return ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
}

void ble_nus_start_adv(void)
{
    if (ble_gap_adv_active()) return;
    start_advertising();
}

void ble_nus_init(void)
{
    // NVS is required by NimBLE bond storage.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.gatts_register_cb = nullptr;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

    // Security: LE Secure Connections + MITM + bonding, display-only.
    ble_hs_cfg.sm_io_cap     = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding    = 1;
    ble_hs_cfg.sm_mitm       = 1;
    ble_hs_cfg.sm_sc         = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    int rc = ble_svc_gap_device_name_set("ClawdWatch");
    if (rc != 0) ESP_LOGE(TAG, "device_name_set rc=%d", rc);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_svc_defs);
    if (rc != 0) ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(s_svc_defs);
    if (rc != 0) ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc);

    // Note: ble_store_config_init() persists bonds across reboots, but
    // ESP-IDF's NimBLE port does not always expose it as a public symbol
    // (it depends on CONFIG_BT_NIMBLE_NVS_PERSIST which we haven't
    // turned on yet). For now bonds live in RAM only — first re-pair
    // after a reset is acceptable while we're still iterating on M4/M5.

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE NUS initialized");
}

}  // extern "C"
