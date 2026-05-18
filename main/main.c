#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "airbnk_gateway.h"

#define TAG "main"

// WiFi configuration (from Kconfig or defaults)
#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY 5

static int s_wifi_retry_num = 0;
static bool s_wifi_connected = false;

// ---------------------------------------------------------------------------
// WiFi event handler
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_wifi_retry_num, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "WiFi connect failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        s_wifi_connected = true;
    }
}

// ---------------------------------------------------------------------------
// Initialize WiFi (STA mode)
// ---------------------------------------------------------------------------
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA init finished, connecting to %s...", WIFI_SSID);

    // Wait for WiFi connection
    while (!s_wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "ESP32 Airbnk BLE-to-MQTT Gateway starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    wifi_init_sta();

    // Configure and start Airbnk Gateway
    airbnk_gateway_config_t cfg = {
        .lock_mac         = CONFIG_AIRBNK_LOCK_MAC,
        .mqtt_broker_uri  = CONFIG_AIRBNK_MQTT_BROKER_URI,
        .mqtt_username    = CONFIG_AIRBNK_MQTT_USERNAME,
        .mqtt_password    = CONFIG_AIRBNK_MQTT_PASSWORD,
        .mqtt_topic_prefix = CONFIG_AIRBNK_MQTT_TOPIC_PREFIX,
        .scan_interval    = 0x80,
        .scan_window      = 0x40,
    };

    ret = airbnk_gateway_start(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Airbnk Gateway: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "Airbnk Gateway running. Lock: %s, MQTT: %s",
             cfg.lock_mac, cfg.mqtt_broker_uri);

    // Gateway runs in the background via its own tasks (NimBLE, MQTT).
    // Main task just keeps alive.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "Gateway heartbeat");
    }
}
