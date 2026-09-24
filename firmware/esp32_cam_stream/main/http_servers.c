#include "app.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http";

static httpd_handle_t s_control_server;
static httpd_handle_t s_stream_server;
static atomic_int s_stream_clients;

static const char *framesize_name(framesize_t fs)
{
    switch (fs) {
    case FRAMESIZE_UXGA: return "1600x1200";
    case FRAMESIZE_SXGA: return "1280x1024";
    case FRAMESIZE_HD: return "1280x720";
    case FRAMESIZE_XGA: return "1024x768";
    case FRAMESIZE_SVGA: return "800x600";
    case FRAMESIZE_VGA: return "640x480";
    case FRAMESIZE_QVGA: return "320x240";
    case FRAMESIZE_QQVGA: return "160x120";
    default: return "unknown";
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app ? app->version : FW_VERSION);
    cJSON_AddStringToObject(root, "device_name", DEVICE_NAME);
    cJSON_AddNumberToObject(root, "proto_version", PROTO_VERSION);
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_spiram",
                            esp_psram_is_initialized() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0);
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());
    cJSON_AddNumberToObject(root, "frames_captured", metrics_frames_captured());
    cJSON_AddNumberToObject(root, "frames_delivered", metrics_frames_delivered());
    cJSON_AddNumberToObject(root, "capture_failures", metrics_capture_failures());
    cJSON_AddNumberToObject(root, "avg_capture_ms", metrics_avg_capture_us() / 1000.0);
    cJSON_AddNumberToObject(root, "last_frame_bytes", metrics_last_frame_bytes());
    cJSON_AddNumberToObject(root, "stream_clients", stream_client_count());
    cJSON_AddStringToObject(root, "resolution", framesize_name(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "quality", camera_current_quality());
    cJSON_AddNumberToObject(root, "fb_count", camera_current_fb_count());
    cJSON_AddNumberToObject(root, "xclk_mhz", camera_current_xclk_mhz());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return err;
}

static bool parse_framesize(const char *name, framesize_t *out)
{
    if (strcmp(name, "qqvga") == 0) { *out = FRAMESIZE_QQVGA; return true; }
    if (strcmp(name, "qvga") == 0) { *out = FRAMESIZE_QVGA; return true; }
    if (strcmp(name, "vga") == 0) { *out = FRAMESIZE_VGA; return true; }
    if (strcmp(name, "svga") == 0) { *out = FRAMESIZE_SVGA; return true; }
    if (strcmp(name, "xga") == 0) { *out = FRAMESIZE_XGA; return true; }
    if (strcmp(name, "hd") == 0) { *out = FRAMESIZE_HD; return true; }
    if (strcmp(name, "sxga") == 0) { *out = FRAMESIZE_SXGA; return true; }
    if (strcmp(name, "uxga") == 0) { *out = FRAMESIZE_UXGA; return true; }
    return false;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor");
        return ESP_FAIL;
    }

    char query[96];
    char val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        const bool has_param =
            httpd_query_key_value(query, "xclk", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "framesize", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK;
        if (has_param && stream_client_count() > 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "stream active: disconnect before config change",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        if (httpd_query_key_value(query, "xclk", val, sizeof(val)) == ESP_OK) {
            if (!camera_set_xclk(atoi(val))) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad xclk (6-27 MHz)");
                return ESP_FAIL;
            }
        }
        if (httpd_query_key_value(query, "framesize", val, sizeof(val)) == ESP_OK) {
            framesize_t fs;
            if (!parse_framesize(val, &fs) || !camera_set_framesize(fs)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "bad framesize (qqvga|qvga|vga|svga|xga|hd|sxga|uxga)");
                return ESP_FAIL;
            }
        }
        if (httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK) {
            if (!camera_set_quality(atoi(val))) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad quality (0-63)");
                return ESP_FAIL;
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "resolution", framesize_name(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "quality", camera_current_quality());
    cJSON_AddNumberToObject(root, "xclk_mhz", camera_current_xclk_mhz());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return err;
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    camera_fb_t *fb = camera_fb_get();
    if (!fb) {
        metrics_capture_failure();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_fb_return(fb);
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    static const char boundary[] = "--FRAME\r\n";
    static const char boundary_cont[] = "\r\n--FRAME\r\n";
    char part_hdr[80];
    bool first = true;
    int consecutive_fails = 0;

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=FRAME");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    atomic_fetch_add(&s_stream_clients, 1);
    metrics_mark_stream_active();

    while (true) {
        int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = camera_fb_get();
        if (!fb) {
            metrics_capture_failure();
            consecutive_fails++;
            if (consecutive_fails > 50) {
                ESP_LOGE(TAG, "stream: too many capture failures, closing client");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        consecutive_fails = 0;
        metrics_record_capture(esp_timer_get_time() - t0, fb->len);

        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)fb->len);
        const char *bnd = first ? boundary : boundary_cont;
        first = false;

        esp_err_t err = httpd_resp_send_chunk(req, bnd, strlen(bnd));
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, part_hdr, hdr_len);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        camera_fb_return(fb);

        if (err != ESP_OK) {
            ESP_LOGI(TAG, "stream client disconnected (%s)", esp_err_to_name(err));
            break;
        }
        metrics_record_delivery();
    }

    atomic_fetch_sub(&s_stream_clients, 1);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

int stream_client_count(void)
{
    return atomic_load(&s_stream_clients);
}

esp_err_t start_control_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONTROL_HTTP_PORT;
    cfg.ctrl_port = 32768;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_control_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "control server failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t status = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    const httpd_uri_t snapshot = {
        .uri = "/api/v1/snapshot",
        .method = HTTP_GET,
        .handler = snapshot_handler,
    };
    const httpd_uri_t config = {
        .uri = "/api/v1/config",
        .method = HTTP_GET,
        .handler = config_handler,
    };
    httpd_register_uri_handler(s_control_server, &status);
    httpd_register_uri_handler(s_control_server, &snapshot);
    httpd_register_uri_handler(s_control_server, &config);
    ESP_LOGI(TAG, "control server on port %d", CONTROL_HTTP_PORT);
    return ESP_OK;
}

esp_err_t start_stream_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = STREAM_HTTP_PORT;
    cfg.ctrl_port = 32769;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&s_stream_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream server failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
    };
    httpd_register_uri_handler(s_stream_server, &stream);
    ESP_LOGI(TAG, "stream server on port %d", STREAM_HTTP_PORT);
    return ESP_OK;
}
