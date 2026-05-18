#include "airbnk_ble.h"
#include "airbnk_utils.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

#define TAG "airbnk_ble"

// Service/characteristic UUIDs for Airbnk lock
#define AIRBNK_SVC_UUID            0xFFF0
#define AIRBNK_CHR_COMMAND_UUID    0xFFF2
#define AIRBNK_CHR_STATUS_UUID     0xFFF3

// BLE connection timeout (ms)
#define BLE_CONNECT_TIMEOUT_MS     5000
#define BLE_DISCOVER_TIMEOUT_MS    3000
#define BLE_WRITE_TIMEOUT_MS       3000
#define BLE_READ_TIMEOUT_MS        3000

// Max retries for command sending
#define CMD_MAX_RETRIES            4

// Event group bits
#define EVT_SCAN_STOPPED    (1 << 0)
#define EVT_CONNECTED       (1 << 1)
#define EVT_DISCONNECTED    (1 << 2)
#define EVT_SVC_FOUND       (1 << 3)
#define EVT_SVC_NOT_FOUND   (1 << 4)
#define EVT_WRITE_DONE      (1 << 5)
#define EVT_WRITE_ERROR     (1 << 6)
#define EVT_READ_DONE       (1 << 7)
#define EVT_READ_ERROR      (1 << 8)
#define EVT_CMD_COMPLETE    (1 << 9)

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static airbnk_ble_config_t g_config;
static bool g_initialized = false;
static bool g_is_sending = false;

// Current connection handle during command execution
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_cmd_chr_handle = 0;
static uint16_t g_status_chr_handle = 0;

// Event group for synchronizing BLE operations
static EventGroupHandle_t g_ble_evt_group = NULL;

// Task handle for NimBLE host task
static TaskHandle_t g_nimble_task = NULL;

// Semaphore for command queue
static SemaphoreHandle_t g_cmd_mutex = NULL;

// Status response buffer
static uint8_t g_status_data[20];
static int g_status_len = 0;

// Scan state
static bool g_scanning = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void nimble_host_task(void *param);
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static int ble_gatt_event_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_error *error, union ble_gatt_data *data,
                              void *arg);
static int do_connect(uint8_t *peer_addr, uint8_t addr_type);
static int do_discover(uint16_t conn_handle);
static int do_write_commands(uint16_t conn_handle,
                              const uint8_t *cmd1, int len1,
                              const uint8_t *cmd2, int len2);
static int do_read_status(uint16_t conn_handle);

// ---------------------------------------------------------------------------
// NimBLE host task
// ---------------------------------------------------------------------------
static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    ESP_LOGI(TAG, "NimBLE host task ended");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// GAP event callback
// ---------------------------------------------------------------------------
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // Advertisement received
        struct ble_hs_adv_fields *fields = &event->disc.data;
        if (fields->mfg_data_len < 2) break;

        // Extract MAC address
        char mac_str[18];
        const uint8_t *addr = event->disc.addr.val;

        // Reverse byte order for display (NimBLE gives little-endian)
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

        // Check if MAC matches target
        if (!airbnk_mac_equals(mac_str, g_config.target_mac)) break;

        // Call user callback
        if (g_config.adv_callback) {
            g_config.adv_callback(mac_str, event->disc.rssi,
                                  fields->mfg_data, fields->mfg_data_len,
                                  g_config.user_ctx);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGD(TAG, "Scan complete, reason=%d", event->disc_complete.reason);
        g_scanning = false;
        xEventGroupSetBits(g_ble_evt_group, EVT_SCAN_STOPPED);
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected to lock");
            g_conn_handle = event->connect.conn_handle;
            xEventGroupSetBits(g_ble_evt_group, EVT_CONNECTED);
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            // Signal failure by setting both connected and disconnected
            xEventGroupSetBits(g_ble_evt_group, EVT_CONNECTED);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_cmd_chr_handle = 0;
        g_status_chr_handle = 0;
        xEventGroupSetBits(g_ble_evt_group, EVT_DISCONNECTED);
        break;

    default:
        break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// GATT event callback (for service/characteristic discovery)
// ---------------------------------------------------------------------------
static int ble_gatt_event_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_error *error, union ble_gatt_data *data,
                              void *arg) {
    (void)arg;
    (void)conn_handle;

    switch (data->hdr.op) {
    case BLE_GATT_ACCESS_OP_DISC_SVC_UUID:
        if (error->status == 0 && data->svc.handle != 0) {
            ESP_LOGD(TAG, "Service FFF0 found, start_handle=0x%04x, end_handle=0x%04x",
                     data->svc.handle, data->svc.end_handle);
            // Store the service handle range
            *(uint16_t *)arg = data->svc.handle;
            *(uint16_t *)((uint8_t *)arg + 2) = data->svc.end_handle;
            xEventGroupSetBits(g_ble_evt_group, EVT_SVC_FOUND);
        } else if (error->status == BLE_HS_EDONE) {
            // Discovery complete with no more services
            if (*(uint16_t *)arg == 0) {
                xEventGroupSetBits(g_ble_evt_group, EVT_SVC_NOT_FOUND);
            }
        } else if (error->status != 0&& error->status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "Service discovery error: %d", error->status);
            xEventGroupSetBits(g_ble_evt_group, EVT_SVC_NOT_FOUND);
        }
        break;

    case BLE_GATT_ACCESS_OP_DISC_CHRS_UUID:
        if (error->status == 0) {
            uint16_t uuid16 = data->chr.chr.decl_uuid16.u16;
            ESP_LOGD(TAG, "Characteristic found: UUID=0x%04X, handle=0x%04x",
                     uuid16, data->chr.chr.val_handle);
            if (uuid16 == AIRBNK_CHR_COMMAND_UUID) {
                g_cmd_chr_handle = data->chr.chr.val_handle;
            } else if (uuid16 == AIRBNK_CHR_STATUS_UUID) {
                g_status_chr_handle = data->chr.chr.val_handle;
            }
        } else if (error->status == BLE_HS_EDONE) {
            // Discovery complete
            ESP_LOGD(TAG, "Characteristic discovery complete: cmd=0x%04x, status=0x%04x",
                     g_cmd_chr_handle, g_status_chr_handle);
            xEventGroupSetBits(g_ble_evt_group, EVT_SVC_FOUND);
        } else if (error->status != 0) {
            ESP_LOGW(TAG, "Characteristic discovery error: %d", error->status);
            xEventGroupSetBits(g_ble_evt_group, EVT_SVC_NOT_FOUND);
        }
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (error->status == 0) {
            xEventGroupSetBits(g_ble_evt_group, EVT_WRITE_DONE);
        } else {
            ESP_LOGW(TAG, "Write error: %d", error->status);
            xEventGroupSetBits(g_ble_evt_group, EVT_WRITE_ERROR);
        }
        break;

    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (error->status == 0) {
            if (data->rd.data_len > 0 && data->rd.data_len <= 20) {
                memcpy(g_status_data, data->rd.data, data->rd.data_len);
                g_status_len = data->rd.data_len;
            }
            xEventGroupSetBits(g_ble_evt_group, EVT_READ_DONE);
        } else {
            ESP_LOGW(TAG, "Read error: %d", error->status);
            xEventGroupSetBits(g_ble_evt_group, EVT_READ_ERROR);
        }
        break;

    default:
        break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Connect to peer
// ---------------------------------------------------------------------------
static int do_connect(uint8_t *peer_addr, uint8_t addr_type) {
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, (ble_addr_t *)peer_addr,
                              BLE_CONNECT_TIMEOUT_MS, NULL, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        return rc;
    }

    EventBits_t bits = xEventGroupWaitBits(g_ble_evt_group,
                                            EVT_CONNECTED | EVT_DISCONNECTED,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(BLE_CONNECT_TIMEOUT_MS + 1000));

    if (bits & EVT_CONNECTED && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Discover service and characteristics
// ---------------------------------------------------------------------------
static int do_discover(uint16_t conn_handle) {
    // Discover command characteristic by UUID
    uint16_t svc_range[2] = {0, 0};

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                         (ble_uuid_t *)BLE_UUID16_DECLARE(AIRBNK_SVC_UUID),
                                         ble_gatt_event_cb, svc_range);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid failed: %d", rc);
        return rc;
    }

    EventBits_t bits = xEventGroupWaitBits(g_ble_evt_group,
                                            EVT_SVC_FOUND | EVT_SVC_NOT_FOUND,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(BLE_DISCOVER_TIMEOUT_MS));

    if (!(bits & EVT_SVC_FOUND) || svc_range[0] == 0) {
        ESP_LOGE(TAG, "Service FFF0 not found");
        return ESP_ERR_NOT_FOUND;
    }

    // Discover all characteristics in the service
    g_cmd_chr_handle = 0;
    g_status_chr_handle = 0;

    rc = ble_gattc_disc_all_chrs(conn_handle, svc_range[0], svc_range[1],
                                  ble_gatt_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_all_chrs failed: %d", rc);
        return rc;
    }

    bits = xEventGroupWaitBits(g_ble_evt_group,
                                EVT_SVC_FOUND | EVT_SVC_NOT_FOUND,
                                pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(BLE_DISCOVER_TIMEOUT_MS));

    if (!(bits & EVT_SVC_FOUND) || g_cmd_chr_handle == 0) {
        ESP_LOGE(TAG, "Characteristics not found (cmd=0x%04x, status=0x%04x)",
                 g_cmd_chr_handle, g_status_chr_handle);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Discovered: svc=0x%04x-0x%04x, cmd_chr=0x%04x, status_chr=0x%04x",
             svc_range[0], svc_range[1], g_cmd_chr_handle, g_status_chr_handle);

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Write two command frames with status check
// ---------------------------------------------------------------------------
static int do_write_commands(uint16_t conn_handle,
                              const uint8_t *cmd1, int len1,
                              const uint8_t *cmd2, int len2) {
    // Write first command frame
    int rc = ble_gattc_write_flat(conn_handle, g_cmd_chr_handle,
                                   cmd1, len1, ble_gatt_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Write cmd1 failed: %d", rc);
        return rc;
    }

    EventBits_t bits = xEventGroupWaitBits(g_ble_evt_group,
                                            EVT_WRITE_DONE | EVT_WRITE_ERROR,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(BLE_WRITE_TIMEOUT_MS));

    if (!(bits & EVT_WRITE_DONE)) {
        ESP_LOGE(TAG, "Write cmd1 did not complete");
        return ESP_FAIL;
    }

    // Small delay between writes
    vTaskDelay(pdMS_TO_TICKS(100));

    // Write second command frame
    rc = ble_gattc_write_flat(conn_handle, g_cmd_chr_handle,
                               cmd2, len2, ble_gatt_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Write cmd2 failed: %d", rc);
        return rc;
    }

    bits = xEventGroupWaitBits(g_ble_evt_group,
                                EVT_WRITE_DONE | EVT_WRITE_ERROR,
                                pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(BLE_WRITE_TIMEOUT_MS));

    if (!(bits & EVT_WRITE_DONE)) {
        ESP_LOGE(TAG, "Write cmd2 did not complete");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read status from FFF3
// ---------------------------------------------------------------------------
static int do_read_status(uint16_t conn_handle) {
    if (g_status_chr_handle == 0) return ESP_ERR_NOT_FOUND;

    // Poll status until we get a non-zero response (up to 10 tries)
    for (int tries = 0; tries < 10; tries++) {
        if (tries > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        memset(g_status_data, 0, sizeof(g_status_data));
        g_status_len = 0;

        int rc = ble_gattc_read(conn_handle, g_status_chr_handle,
                                 ble_gatt_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Read status failed: %d", rc);
            return rc;
        }

        EventBits_t bits = xEventGroupWaitBits(g_ble_evt_group,
                                                EVT_READ_DONE | EVT_READ_ERROR,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(BLE_READ_TIMEOUT_MS));

        if (bits & EVT_READ_DONE) {
            // Check if status is all zeros (not ready yet)
            bool all_zero = true;
            for (int i = 0; i < g_status_len; i++) {
                if (g_status_data[i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if (!all_zero || g_status_len == 0) {
                ESP_LOGD(TAG, "Status read OK: %d bytes", g_status_len);
                return ESP_OK;
            }
            ESP_LOGD(TAG, "Status all zeros, retry %d", tries + 1);
        } else {
            ESP_LOGW(TAG, "Read status attempt %d failed", tries + 1);
            return ESP_FAIL;
        }
    }
    // If we got here, all tries returned zeros - treat as OK
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int airbnk_ble_init(const airbnk_ble_config_t *config) {
    if (config == NULL || config->target_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&g_config, config, sizeof(airbnk_ble_config_t));

    // Create synchronization primitives
    g_ble_evt_group = xEventGroupCreate();
    if (g_ble_evt_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    g_cmd_mutex = xSemaphoreCreateMutex();
    if (g_cmd_mutex == NULL) {
        vEventGroupDelete(g_ble_evt_group);
        return ESP_ERR_NO_MEM;
    }

    // Initialize ESP Bluetooth controller and NimBLE
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %d", ret);
        return ret;
    }

    ret = esp_nimble_hci_and_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_hci_and_controller_init failed: %d", ret);
        return ret;
    }

    nimble_port_init();

    // Configure GAP
    ble_svc_gap_device_name_set("AirbnkGW");

    // Register GAP event callback
    ble_gap_event_register(BLE_GAP_EVENT_DISC, ble_gap_event_cb, NULL);
    ble_gap_event_register(BLE_GAP_EVENT_CONNECT, ble_gap_event_cb, NULL);
    ble_gap_event_register(BLE_GAP_EVENT_DISCONNECT, ble_gap_event_cb, NULL);
    ble_gap_event_register(BLE_GAP_EVENT_DISC_COMPLETE, ble_gap_event_cb, NULL);

    // Start NimBLE host task
    nimble_port_freertos_init(nimble_host_task);

    g_initialized = true;
    ESP_LOGI(TAG, "BLE initialized, target MAC: %s", g_config.target_mac);

    // Start scanning
    return airbnk_ble_start_scan();
}

int airbnk_ble_deinit(void) {
    if (!g_initialized) return ESP_OK;

    g_initialized = false;

    if (g_scanning) {
        ble_gap_disc_cancel();
    }

    // NimBLE deinit
    nimble_port_stop();
    nimble_port_deinit();
    esp_nimble_hci_and_controller_deinit();

    if (g_cmd_mutex) {
        vSemaphoreDelete(g_cmd_mutex);
        g_cmd_mutex = NULL;
    }
    if (g_ble_evt_group) {
        vEventGroupDelete(g_ble_evt_group);
        g_ble_evt_group = NULL;
    }

    return ESP_OK;
}

int airbnk_ble_start_scan(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;

    if (g_scanning) return ESP_OK;

    struct ble_gap_disc_params scan_params = {
        .itvl          = g_config.scan_interval ? g_config.scan_interval : 0x80,
        .window        = g_config.scan_window ? g_config.scan_window : 0x40,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited       = 0,
        .passive       = g_config.active_scan ? 0 : 1,
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                           &scan_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return ESP_FAIL;
    }

    g_scanning = true;
    ESP_LOGI(TAG, "BLE scanning started");
    return ESP_OK;
}

int airbnk_ble_stop_scan(void) {
    if (!g_scanning) return ESP_OK;

    int rc = ble_gap_disc_cancel();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc_cancel failed: %d", rc);
        return ESP_FAIL;
    }

    // Wait for scan to actually stop
    xEventGroupWaitBits(g_ble_evt_group, EVT_SCAN_STOPPED,
                         pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));

    g_scanning = false;
    ESP_LOGI(TAG, "BLE scanning stopped");
    return ESP_OK;
}

int airbnk_ble_send_command(const char *command1_hex, const char *command2_hex, int sign) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (command1_hex == NULL || command2_hex == NULL) return ESP_ERR_INVALID_ARG;

    // Serialize command access
    if (xSemaphoreTake(g_cmd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Command mutex busy (another command in progress?)");
        return ESP_ERR_TIMEOUT;
    }

    g_is_sending = true;

    // Decode hex commands
    uint8_t cmd1[20], cmd2[20];
    int len1 = airbnk_from_hex(cmd1, command1_hex, 20);
    int len2 = airbnk_from_hex(cmd2, command2_hex, 20);

    if (len1 == 0 || len2 == 0) {
        ESP_LOGE(TAG, "Invalid hex command frames");
        g_is_sending = false;
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse target MAC
    uint8_t peer_addr[6];
    if (sscanf(g_config.target_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &peer_addr[5], &peer_addr[4], &peer_addr[3],
               &peer_addr[2], &peer_addr[1], &peer_addr[0]) != 6) {
        ESP_LOGE(TAG, "Invalid target MAC format");
        g_is_sending = false;
        xSemaphoreGive(g_cmd_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop scanning while sending
    airbnk_ble_stop_scan();

    int result = ESP_FAIL;
    bool success = false;
    char status_hex[41] = {0};

    for (int retry = 0; retry < CMD_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGD(TAG, "Retry %d/%d", retry, CMD_MAX_RETRIES - 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Reset event bits for this attempt
        xEventGroupClearBits(g_ble_evt_group,
                              EVT_CONNECTED | EVT_DISCONNECTED |
                              EVT_SVC_FOUND | EVT_SVC_NOT_FOUND |
                              EVT_WRITE_DONE | EVT_WRITE_ERROR |
                              EVT_READ_DONE | EVT_READ_ERROR);

        // Connect
        result = do_connect(peer_addr, BLE_ADDR_PUBLIC);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Connect failed (attempt %d)", retry + 1);
            continue;
        }

        // Discover service and characteristics
        result = do_discover(g_conn_handle);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Discover failed (attempt %d)", retry + 1);
            ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            xEventGroupWaitBits(g_ble_evt_group, EVT_DISCONNECTED,
                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
            continue;
        }

        // Write commands
        result = do_write_commands(g_conn_handle, cmd1, len1, cmd2, len2);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Write failed (attempt %d)", retry + 1);
            ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            xEventGroupWaitBits(g_ble_evt_group, EVT_DISCONNECTED,
                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
            continue;
        }

        // Read status
        result = do_read_status(g_conn_handle);
        if (result == ESP_OK && g_status_len > 0) {
            airbnk_to_hex(status_hex, g_status_data, g_status_len);
        }

        success = (result == ESP_OK);
        break;
    }

    // Disconnect
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(g_ble_evt_group, EVT_DISCONNECTED,
                             pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    }

    // Notify via callback
    if (g_config.cmd_result_callback) {
        g_config.cmd_result_callback(success,
                                      success ? status_hex : NULL,
                                      g_config.user_ctx);
    }

    g_is_sending = false;
    xSemaphoreGive(g_cmd_mutex);

    // Resume scanning
    airbnk_ble_start_scan();

    return success ? ESP_OK : ESP_FAIL;
}

bool airbnk_ble_is_busy(void) {
    return g_is_sending;
}
