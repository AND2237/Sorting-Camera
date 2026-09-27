#include "app.h"
#include "auth.h"
#include "camera_control.h"
#include "discovery.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "config_secrets.h"

static const char *TAG = "main";

static char s_device_id[16];
static char s_device_ip[16];

const char *device_id_hex(void)
{
    if (s_device_id[0] == '\0') {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
            esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        }
        snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }
    return s_device_id;
}

const char *device_ip(void)
{
    if (s_device_ip[0] == '\0') {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &info) == ESP_OK) {
            snprintf(s_device_ip, sizeof(s_device_ip), IPSTR, IP2STR(&info.ip));
        } else {
            snprintf(s_device_ip, sizeof(s_device_ip), "%s", AP_IP);
        }
    }
    return s_device_ip;
}

static void stall_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (stream_client_count() + frame_transport_client_count() > 0) {
            int64_t since = metrics_us_since_last_delivery();
            if (since > STREAM_STALL_TIMEOUT_US) {
                ESP_LOGE(TAG, "stream stalled for %lld ms with active client, recovering camera",
                         (long long)(since / 1000));
                if (camera_recover() == ESP_OK) {
                    metrics_mark_stream_active();
                    continue;
                }
                ESP_LOGE(TAG, "camera recovery failed, restarting");
                esp_restart();
            }
        }
        const int clients = stream_client_count() + frame_transport_client_count();
        const int64_t since_capture = metrics_us_since_last_capture();
        if (clients > 0 && since_capture > CAMERA_DEAD_TIMEOUT_US) {
            ESP_LOGE(TAG, "no frame captured for %lld ms with active client, recovering camera",
                     (long long)(since_capture / 1000));
            if (camera_recover() == ESP_OK) {
                continue;
            }
            ESP_LOGE(TAG, "camera recovery failed, restarting");
            esp_restart();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "sorting-cam firmware %s (proto v%d)", FW_VERSION, PROTO_VERSION);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    metrics_start();

    if (!wifi_start(AP_SSID, AP_PASSWORD)) {
        ESP_LOGE(TAG, "softAP not started; streaming unavailable until AP_SSID/AP_PASSWORD are set in config_secrets.h");
    }

    for (int attempt = 1; attempt <= 3; attempt++) {
        if (camera_init()) {
            break;
        }
        ESP_LOGE(TAG, "camera init attempt %d failed", attempt);
        if (attempt == 3) {
            ESP_LOGE(TAG, "camera unavailable, rebooting in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    camera_control_init();

    if (auth_init() != ESP_OK) {
        ESP_LOGW(TAG, "control API running WITHOUT authentication");
    }

    ESP_ERROR_CHECK(start_control_server());
    ESP_ERROR_CHECK(start_stream_server());
    discovery_start();
#if CONFIG_SORTING_CAM_FRAME_TRANSPORT
    if (!frame_transport_start()) {
        ESP_LOGE(TAG, "tcp/udp frame transport failed to start (http baseline still available)");
    }
#else
    ESP_LOGI(TAG, "raw tcp/udp frame transport disabled (http baseline only)");
#endif

    xTaskCreate(stall_watchdog_task, "stall_wd", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG,
             "ready: join Wi-Fi \"%s\" then open http://%s/  stream=:%d/stream status=:%d/api/v1/status snapshot=:%d/api/v1/snapshot",
             AP_SSID, AP_IP, STREAM_HTTP_PORT, CONTROL_HTTP_PORT, CONTROL_HTTP_PORT);
}
