/*
 * airbnk_lock.cpp — ESP-IDF NimBLE BLE stack for Airbnk locks
 *
 *   1. Init: NVS → nimble_port_init → GAP config → host task
 *   2. Passive scan for BABA advertisements
 *   3. GATT: connect → discover FFF0 → write frame1+frame2 on FFF2 → disconnect
 *
 * ESP-IDF NimBLE 5.5.4 only — no Arduino, no Bluedroid.
 */
#include "airbnk_lock.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"

#include <cstring>

static const char *TAG = "airbnk_lock";

#define MAX_RETRIES      10
#define RETRY_DELAY_MS   500

#define SVC_UUID      0xFFF0
#define WRITE_CHR     0xFFF2
#define BABA_M0       0xBA
#define BABA_M1       0xBA

/* ── Globals ───────────────────────────────────────────────────────────── */

static airbnk_advert_cb_t g_adv_cb = nullptr;
static bool g_scanning = false;

typedef struct {
    uint8_t   mac[6];
    uint16_t  conn_handle;
    uint16_t  fff2_handle;
    uint8_t   frame1[40];
    size_t    frame1_len;
    uint8_t   frame2[40];
    size_t    frame2_len;
    airbnk_cmd_done_t on_done;
    int       retries;
    int       phase;  // 1=disc svc, 2=disc chrs, 3=write f1, 4=write f2
    bool      active;
} gatt_t;

static gatt_t g_gatt;

/* ── Fwd decl ──────────────────────────────────────────────────────────── */

static int gap_cb(struct ble_gap_event *e, void *arg);
static int gatt_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_svc *svc, void *arg);
static int gatt_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_chr *chr, void *arg);
static int gatt_write_cb(uint16_t conn, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg);
static void sync_cb(void);
static void reset_cb(int reason);

/* ── NimBLE host task ──────────────────────────────────────────────────── */

static void host_task(void *p) {
    ESP_LOGI(TAG, "Host task start");
    nimble_port_run();
    ESP_LOGI(TAG, "Host task exit");
}

/* ── Init / Deinit ─────────────────────────────────────────────────────── */

int airbnk_nimble_init(void) {
    ESP_LOGI(TAG, "Init NimBLE");

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    if (r != ESP_OK) { ESP_LOGE(TAG, "NVS err %d", (int)r); return -1; }

    nimble_port_init();
    ble_svc_gap_device_name_set("airbnk-ble");
    ble_hs_cfg.reset_cb = reset_cb;
    ble_hs_cfg.sync_cb  = sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    memset(&g_gatt, 0, sizeof(g_gatt));
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Init done");
    return 0;
}

void airbnk_nimble_deinit(void) {
    g_scanning = false;
    g_adv_cb = nullptr;
    nimble_port_stop();
}

/* ── Scan ──────────────────────────────────────────────────────────────── */

int airbnk_nimble_start_scan(airbnk_advert_cb_t cb) {
    if (!cb) return -1;
    g_adv_cb = cb;
    if (g_scanning) return 0;
    int rc = ble_gap_disc(0, BLE_HS_FOREVER, nullptr, gap_cb, nullptr);
    if (rc == 0) { g_scanning = true; ESP_LOGI(TAG, "Scan started"); }
    else ESP_LOGE(TAG, "Scan fail %d", rc);
    return rc;
}

void airbnk_nimble_stop_scan(void) {
    g_scanning = false;
    ble_gap_disc_cancel();
}

/* ── Sync/reset ────────────────────────────────────────────────────────── */

static void sync_cb(void) {
    ESP_LOGI(TAG, "Synced");
    if (g_adv_cb && !g_scanning) {
        int rc = ble_gap_disc(0, BLE_HS_FOREVER, nullptr, gap_cb, nullptr);
        if (rc == 0) g_scanning = true;
    }
}

static void reset_cb(int reason) {
    ESP_LOGW(TAG, "Reset reason=%d", reason);
}

/* ── GAP handler ───────────────────────────────────────────────────────── */

static int gap_cb(struct ble_gap_event *e, void *arg) {
    (void)arg;

    switch (e->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, e->disc.data, e->disc.length_data) != 0)
            return 0;

        if (f.mfg_data_len < 18 ||
            f.mfg_data[0] != BABA_M0 || f.mfg_data[1] != BABA_M1)
            return 0;

        airbnk_advert_data_t adv;
        if (airbnk_parse_advertisement(f.mfg_data, f.mfg_data_len,
                                       e->disc.addr.val, e->disc.rssi, &adv))
        {
            if (g_adv_cb) g_adv_cb(&adv);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (g_scanning)
            ble_gap_disc(0, BLE_HS_FOREVER, nullptr, gap_cb, nullptr);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (g_gatt.active && g_gatt.phase == 0) {
            g_gatt.conn_handle = e->connect.conn_handle;
            if (e->connect.status == 0) {
                g_gatt.phase = 1;
                ble_uuid16_t u = BLE_UUID16_INIT(SVC_UUID);
                ble_gattc_disc_svc_by_uuid(g_gatt.conn_handle, &u.u,
                                           gatt_disc_svc_cb, &g_gatt);
            } else {
                ESP_LOGE(TAG, "Conn fail %d", e->connect.status);
                if (g_gatt.on_done) g_gatt.on_done(false);
                g_gatt.active = false;
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (g_gatt.active) {
            if (g_gatt.phase == 5) {
                if (g_gatt.on_done) g_gatt.on_done(true);
            } else {
                ESP_LOGE(TAG, "Unexpected disc phase=%d", g_gatt.phase);
                if (g_gatt.on_done) g_gatt.on_done(false);
            }
            g_gatt.active = false;
        }
        return 0;

    default:
        return 0;
    }
}

/* ── GATT handlers ─────────────────────────────────────────────────────── */

static int gatt_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_svc *svc, void *arg)
{
    gatt_t *ctx = (gatt_t *)arg;
    if (ctx->conn_handle != conn) return 0;

    if (err->status != 0) {
        ESP_LOGE(TAG, "Svc disc err %d", err->status);
        ctx->phase = -1;
        if (ctx->on_done) ctx->on_done(false);
        ctx->active = false;
        return 0;
    }

    if (!svc) return 0;  /* discovery complete with no more results */

    ESP_LOGI(TAG, "FFF0 found: %d-%d", svc->start_handle, svc->end_handle);
    ctx->phase = 2;
    ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                            gatt_disc_chr_cb, ctx);
    return 0;
}

static int gatt_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_chr *chr, void *arg)
{
    gatt_t *ctx = (gatt_t *)arg;
    if (ctx->conn_handle != conn) return 0;

    if (err->status == 0 && chr) {
        /* Check UUID: we need FFF2 */
        if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
            chr->uuid.u16.value == WRITE_CHR)
        {
            /* Characteristic value handle is chr->val_handle */
            ctx->fff2_handle = chr->val_handle;
            ESP_LOGI(TAG, "FFF2 found: def_handle=%d val_handle=%d",
                     chr->def_handle, chr->val_handle);
        }
        return 0;
    }

    /* Discovery complete (err->status == BLE_HS_EDONE or 0 with null chr) */
    if (err->status == BLE_HS_EDONE || err->status == 0) {
        if (ctx->fff2_handle == 0) {
            ESP_LOGE(TAG, "FFF2 not found");
            if (ctx->on_done) ctx->on_done(false);
            ctx->active = false;
            return 0;
        }
        ESP_LOGI(TAG, "Write frame1 (%zu bytes) to handle %d",
                 ctx->frame1_len, ctx->fff2_handle);
        ctx->retries = 0;
        ctx->phase = 3;
        ble_gattc_write_flat(conn, ctx->fff2_handle,
                             ctx->frame1, ctx->frame1_len,
                             gatt_write_cb, ctx);
    }
    return 0;
}

static int gatt_write_cb(uint16_t conn, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg)
{
    gatt_t *ctx = (gatt_t *)arg;
    (void)attr;
    if (ctx->conn_handle != conn) return 0;

    switch (ctx->phase) {

    case 3: {  /* frame1 sent */
        if (err->status == 0) {
            ESP_LOGI(TAG, "Frame1 OK");
            vTaskDelay(pdMS_TO_TICKS(100));
            ctx->phase = 4;
            ctx->retries = 0;
            ble_gattc_write_flat(conn, ctx->fff2_handle,
                                 ctx->frame2, ctx->frame2_len,
                                 gatt_write_cb, ctx);
        } else if (ctx->retries < MAX_RETRIES) {
            ctx->retries++;
            ESP_LOGW(TAG, "Frame1 retry %d/%d", ctx->retries, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            ble_gattc_write_flat(conn, ctx->fff2_handle,
                                 ctx->frame1, ctx->frame1_len,
                                 gatt_write_cb, ctx);
        } else {
            ESP_LOGE(TAG, "Frame1 failed");
            if (ctx->on_done) ctx->on_done(false);
            ctx->active = false;
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    case 4: {  /* frame2 sent */
        if (err->status == 0) {
            ESP_LOGI(TAG, "Frame2 OK — done");
            ctx->phase = 5;
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        } else if (ctx->retries < MAX_RETRIES) {
            ctx->retries++;
            ESP_LOGW(TAG, "Frame2 retry %d/%d", ctx->retries, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            ble_gattc_write_flat(conn, ctx->fff2_handle,
                                 ctx->frame2, ctx->frame2_len,
                                 gatt_write_cb, ctx);
        } else {
            ESP_LOGE(TAG, "Frame2 failed");
            if (ctx->on_done) ctx->on_done(false);
            ctx->active = false;
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ── Hex encode ────────────────────────────────────────────────────────── */

static void hexenc(const uint8_t *in, size_t n, char *out) {
    static const char h[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = h[(in[i]>>4)&0x0F];
        out[i*2+1] = h[in[i]&0x0F];
    }
    out[n*2] = '\0';
}

/* ── Send command ──────────────────────────────────────────────────────── */

int airbnk_lock_send_command(const airbnk_cmd_t *cmd) {
    if (!cmd || g_gatt.active) {
        ESP_LOGW(TAG, "Busy or null cmd");
        return -1;
    }

    char hex[73];
    hexenc(cmd->opcode, 36, hex);

    memcpy(g_gatt.frame1, "FF00", 4);
    memcpy(g_gatt.frame1 + 4, hex, 36);
    g_gatt.frame1_len = 40;

    memcpy(g_gatt.frame2, "FF01", 4);
    memcpy(g_gatt.frame2 + 4, hex + 36, 36);
    g_gatt.frame2_len = 40;

    memcpy(g_gatt.mac, cmd->mac_address, 6);
    g_gatt.conn_handle = 0;
    g_gatt.fff2_handle  = 0;
    g_gatt.phase        = 0;
    g_gatt.on_done      = cmd->on_done;
    g_gatt.retries      = 0;
    g_gatt.active       = true;

    ESP_LOGI(TAG, "Connect %02X:%02X:%02X:%02X:%02X:%02X",
             cmd->mac_address[0], cmd->mac_address[1], cmd->mac_address[2],
             cmd->mac_address[3], cmd->mac_address[4], cmd->mac_address[5]);

    ble_addr_t addr;
    addr.type = BLE_ADDR_PUBLIC;
    memcpy(addr.val, cmd->mac_address, 6);

    int rc = ble_gap_connect(0, &addr, 3000, nullptr, gap_cb, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "Connect fail %d", rc);
        g_gatt.active = false;
        return -1;
    }
    return 0;
}

bool airbnk_lock_is_busy(void) { return g_gatt.active; }

void airbnk_lock_cancel(void) {
    if (g_gatt.active) {
        if (g_gatt.conn_handle)
            ble_gap_terminate(g_gatt.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (g_gatt.on_done) g_gatt.on_done(false);
        g_gatt.active = false;
    }
}
