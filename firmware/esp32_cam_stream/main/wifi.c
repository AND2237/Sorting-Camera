#include "app.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static const char *TAG = "wifi";

static bool s_started;

bool wifi_start(const char *ssid, const char *password)
{
    if (s_started) {
        return true;
    }
    if (!ssid || !password || strlen(ssid) == 0 || strlen(password) < 8
        || strcmp(ssid, "YOUR_AP_SSID") == 0) {
        ESP_LOGE(TAG,
                 "AP credentials not configured. Set AP_SSID / AP_PASSWORD (password >= 8 chars) in config_secrets.h.");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.ap.ssid, ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    strncpy((char *)wifi_cfg.ap.password, password, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.channel = AP_CHANNEL;
    wifi_cfg.ap.max_connection = AP_MAX_CLIENTS;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    ESP_LOGI(TAG, "softAP \"%s\" up: ip=%s channel=%d max_conn=%d (WPA2)",
             ssid, AP_IP, AP_CHANNEL, AP_MAX_CLIENTS);
    return true;
}

int wifi_get_rssi(void)
{
    wifi_sta_list_t sta_list = {0};
    if (!s_started || esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num <= 0) {
        return 0;
    }
    int best = sta_list.sta[0].rssi;
    for (int i = 1; i < sta_list.num; i++) {
        if (sta_list.sta[i].rssi > best) {
            best = sta_list.sta[i].rssi;
        }
    }
    return best;
}
