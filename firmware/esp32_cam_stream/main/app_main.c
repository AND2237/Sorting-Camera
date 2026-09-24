#include "app.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config_secrets.h"

static const char *TAG = "main";

static void stall_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (stream_client_count() > 0) {
            int64_t since = metrics_us_since_last_delivery();
            if (since > STREAM_STALL_TIMEOUT_US) {
                ESP_LOGE(TAG, "stream stalled for %lld ms with active client, restarting",
                         (long long)(since / 1000));
                esp_restart();
            }
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

    ESP_ERROR_CHECK(start_control_server());
    ESP_ERROR_CHECK(start_stream_server());

    xTaskCreate(stall_watchdog_task, "stall_wd", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG,
             "ready: join Wi-Fi \"%s\" then open http://%s/  stream=:%d/stream status=:%d/api/v1/status snapshot=:%d/api/v1/snapshot",
             AP_SSID, AP_IP, STREAM_HTTP_PORT, CONTROL_HTTP_PORT, CONTROL_HTTP_PORT);
}
