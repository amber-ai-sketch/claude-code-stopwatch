/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Minimal HID-over-GATT keyboard. Uses the standard boot keyboard
 * report descriptor — Apple, Linux, Android all accept it.
 */
#include "ble_hid.h"

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char* TAG = "ble_hid";

// ─── Standard 16-bit UUIDs from BT-SIG ────────────────────────────────
// We can't use BLE_UUID16_DECLARE() inside the static service table
// because in C++ that macro returns an rvalue (taking address forbidden
// under -fpermissive). Predefine the UUIDs as static lvalues instead.
static const ble_uuid16_t UUID_SVC_HID            = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t UUID_CHR_HID_INFO       = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t UUID_CHR_REPORT_MAP     = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t UUID_CHR_HID_CTRL       = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t UUID_CHR_REPORT         = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t UUID_CHR_PROTOCOL_MODE  = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t UUID_DSC_REPORT_REF     = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t UUID_SVC_DEV_INFO       = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t UUID_CHR_PNP_ID         = BLE_UUID16_INIT(0x2A50);
static const ble_uuid16_t UUID_CHR_MFG_NAME       = BLE_UUID16_INIT(0x2A29);

// ─── State ────────────────────────────────────────────────────────────
static uint16_t s_conn_handle = 0xffff;
static uint16_t s_input_handle = 0;
static bool     s_subscribed = false;

// HID Information characteristic (4 bytes, fixed):
//   [bcdHID lo, bcdHID hi, country, flags]
//   bcdHID 0x0111 = HID 1.11; country 0x00 = not localized;
//   flags bit0 RemoteWake, bit1 NormallyConnectable.
static const uint8_t s_hid_info[] = {0x11, 0x01, 0x00, 0x02};

// Protocol Mode: 1 = Report (the only mode we support).
static uint8_t s_protocol_mode = 1;

// Standard boot keyboard Report Descriptor — Mac/iOS verified.
// 8-byte input report: [modifier, reserved, k1..k6]
static const uint8_t s_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Usage Minimum (LCtrl)
    0x29, 0xE7,        //   Usage Maximum (RGUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Var, Abs) — 8 modifier bits
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) — reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) — 6 keycode slots
    0xC0,              // End Collection
};

// Input report descriptor: [report_id=1, type=1=Input]
static const uint8_t s_input_ref[] = {0x01, 0x01};

// PnP ID: vendor source=USB(0x02), VID=0xE502 (random), PID=0x0001, version=0x0001
static const uint8_t s_pnp_id[] = {0x02, 0x02, 0xE5, 0x01, 0x00, 0x01, 0x00};
static const char    s_mfg_name[] = "clawd-watch";

// ─── GATT access callbacks (one per characteristic, KISS over generic) ─
static int access_hid_info(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info));
}
static int access_report_map(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map));
}
static int access_protocol_mode(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Host writes 0 (boot) or 1 (report). We always operate in
        // report mode regardless — just acknowledge.
        return 0;
    }
    return os_mbuf_append(ctxt->om, &s_protocol_mode, 1);
}
static int access_hid_control(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Suspend / Exit suspend — ignored.
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int access_input_report(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Mac reads the input report on subscribe; return zeros.
        const uint8_t empty[8] = {0};
        return os_mbuf_append(ctxt->om, empty, sizeof(empty));
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int access_input_ref_dsc(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    return os_mbuf_append(ctxt->om, s_input_ref, sizeof(s_input_ref));
}
static int access_pnp_id(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    return os_mbuf_append(ctxt->om, s_pnp_id, sizeof(s_pnp_id));
}
static int access_mfg_name(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*)
{
    return os_mbuf_append(ctxt->om, s_mfg_name, sizeof(s_mfg_name) - 1);
}

// ─── GATT service definitions ────────────────────────────────────────

// Report Reference descriptor on the Input Report char.
static const struct ble_gatt_dsc_def s_input_dscs[] = {
    {
        /* uuid       */ &UUID_DSC_REPORT_REF.u,
        /* att_flags  */ BLE_ATT_F_READ,
        /* min_key_size */ 0,
        /* access_cb  */ access_input_ref_dsc,
        /* arg        */ nullptr,
    },
    {  // terminator
        nullptr, 0, 0, nullptr, nullptr,
    },
};

static const struct ble_gatt_chr_def s_hid_chrs[] = {
    {  // HID Information
        &UUID_CHR_HID_INFO.u, access_hid_info, nullptr, nullptr,
        BLE_GATT_CHR_F_READ, 0, nullptr, nullptr,
    },
    {  // Report Map
        &UUID_CHR_REPORT_MAP.u, access_report_map, nullptr, nullptr,
        BLE_GATT_CHR_F_READ, 0, nullptr, nullptr,
    },
    {  // HID Control Point
        &UUID_CHR_HID_CTRL.u, access_hid_control, nullptr, nullptr,
        BLE_GATT_CHR_F_WRITE_NO_RSP, 0, nullptr, nullptr,
    },
    {  // Protocol Mode
        &UUID_CHR_PROTOCOL_MODE.u, access_protocol_mode, nullptr, nullptr,
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP, 0, nullptr, nullptr,
    },
    {  // Input Report (notifications carry our keystrokes)
        &UUID_CHR_REPORT.u, access_input_report, nullptr,
        const_cast<struct ble_gatt_dsc_def*>(s_input_dscs),
        BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, 0,
        &s_input_handle, nullptr,
    },
    {  // terminator
        nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr,
    },
};

static const struct ble_gatt_chr_def s_devinfo_chrs[] = {
    {  // PnP ID
        &UUID_CHR_PNP_ID.u, access_pnp_id, nullptr, nullptr,
        BLE_GATT_CHR_F_READ, 0, nullptr, nullptr,
    },
    {  // Manufacturer Name
        &UUID_CHR_MFG_NAME.u, access_mfg_name, nullptr, nullptr,
        BLE_GATT_CHR_F_READ, 0, nullptr, nullptr,
    },
    {  // terminator
        nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr,
    },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        BLE_GATT_SVC_TYPE_PRIMARY, &UUID_SVC_HID.u, nullptr, s_hid_chrs,
    },
    {
        BLE_GATT_SVC_TYPE_PRIMARY, &UUID_SVC_DEV_INFO.u, nullptr, s_devinfo_chrs,
    },
    {  // terminator
        0, nullptr, nullptr, nullptr,
    },
};

extern "C" {

const struct ble_gatt_svc_def* ble_hid_service_defs(void)
{
    return s_svcs;
}

void ble_hid_set_conn(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    if (conn_handle == 0xffff) s_subscribed = false;
}

void ble_hid_on_subscribe(uint16_t attr_handle, bool subscribed)
{
    if (attr_handle == s_input_handle) {
        s_subscribed = subscribed;
        ESP_LOGI(TAG, "input report subscribed=%d", (int)subscribed);
    }
}

int ble_hid_press(uint8_t modifier, uint8_t keycode)
{
    if (s_conn_handle == 0xffff || !s_subscribed) return -1;
    uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    struct os_mbuf* om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (!om) return -2;
    return ble_gatts_notify_custom(s_conn_handle, s_input_handle, om);
}

int ble_hid_release(void)
{
    if (s_conn_handle == 0xffff || !s_subscribed) return -1;
    uint8_t report[8] = {0};
    struct os_mbuf* om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (!om) return -2;
    return ble_gatts_notify_custom(s_conn_handle, s_input_handle, om);
}

}  // extern "C"
