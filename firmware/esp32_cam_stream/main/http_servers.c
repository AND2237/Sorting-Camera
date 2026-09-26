#include "app.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

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
    cJSON_AddNumberToObject(root, "reset_reason", (int)esp_reset_reason());
    cJSON_AddStringToObject(root, "resolution", framesize_name(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "quality", camera_current_quality());
    cJSON_AddNumberToObject(root, "fb_count", camera_current_fb_count());
    cJSON_AddNumberToObject(root, "xclk_mhz", camera_current_xclk_mhz());
    cJSON_AddStringToObject(root, "grab_mode",
                            camera_current_grab_mode() == CAMERA_GRAB_LATEST
                                ? "latest" : "when_empty");
    cJSON_AddStringToObject(root, "fb_location",
                            camera_current_fb_location() == CAMERA_FB_IN_DRAM
                                ? "dram" : "psram");
    cJSON_AddNumberToObject(root, "tcp_clients", frame_transport_tcp_clients());
    cJSON_AddNumberToObject(root, "udp_peer", frame_transport_udp_peer());
    cJSON_AddNumberToObject(root, "udp_tx_dgrams", frame_transport_udp_tx_dgrams());
    cJSON_AddNumberToObject(root, "udp_tx_drops", frame_transport_udp_tx_drops());

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
    int64_t t_config = esp_timer_get_time();

    char query[192];
    char val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        const bool has_param =
            httpd_query_key_value(query, "xclk", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "framesize", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "fb_count", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "grab", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "fbloc", val, sizeof(val)) == ESP_OK;
        if (has_param && (stream_client_count() + frame_transport_client_count()) > 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "stream active: disconnect before config change",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        framesize_t fs = camera_current_framesize();
        int quality = camera_current_quality();
        int xclk = camera_current_xclk_mhz();
        int fb = camera_current_fb_count();
        camera_grab_mode_t grab = camera_current_grab_mode();
        camera_fb_location_t loc = camera_current_fb_location();
        bool any = false;

        if (httpd_query_key_value(query, "xclk", val, sizeof(val)) == ESP_OK) {
            xclk = atoi(val);
            if (xclk < 6 || xclk > 27) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad xclk (6-27 MHz)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (httpd_query_key_value(query, "framesize", val, sizeof(val)) == ESP_OK) {
            if (!parse_framesize(val, &fs)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "bad framesize (qqvga|qvga|vga|svga|xga|hd|sxga|uxga)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK) {
            quality = atoi(val);
            if (quality < 0 || quality > 63) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad quality (0-63)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (httpd_query_key_value(query, "fb_count", val, sizeof(val)) == ESP_OK) {
            fb = atoi(val);
            if (fb < 1 || fb > 3) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fb_count (1-3)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (httpd_query_key_value(query, "grab", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "latest") == 0) {
                grab = CAMERA_GRAB_LATEST;
            } else if (strcmp(val, "cont") == 0) {
                grab = CAMERA_GRAB_WHEN_EMPTY;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad grab (latest|cont)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (httpd_query_key_value(query, "fbloc", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "psram") == 0) {
                loc = CAMERA_FB_IN_PSRAM;
            } else if (strcmp(val, "dram") == 0) {
                loc = CAMERA_FB_IN_DRAM;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fbloc (psram|dram)");
                return ESP_FAIL;
            }
            any = true;
        }
        if (any) {
            esp_err_t aerr = camera_apply_config(fs, quality, xclk, fb, grab, loc);
            if (aerr != ESP_OK) {
                char msg[96];
                snprintf(msg, sizeof(msg), "camera apply failed: %s",
                         esp_err_to_name(aerr));
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
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
    cJSON_AddNumberToObject(root, "fb_count", camera_current_fb_count());
    cJSON_AddStringToObject(root, "grab_mode",
                            camera_current_grab_mode() == CAMERA_GRAB_LATEST
                                ? "latest" : "when_empty");
    cJSON_AddStringToObject(root, "fb_location",
                            camera_current_fb_location() == CAMERA_FB_IN_DRAM
                                ? "dram" : "psram");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "config applied in %lld ms",
             (long long)((esp_timer_get_time() - t_config) / 1000));

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
    char part_hdr[128];
    bool first = true;
    int consecutive_fails = 0;
    const int sockfd = httpd_req_to_sockfd(req);

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=FRAME");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    atomic_fetch_add(&s_stream_clients, 1);
    metrics_mark_stream_active();

    while (true) {
        if (sockfd >= 0) {
            char probe;
            int r = recv(sockfd, &probe, 1, MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ESP_LOGI(TAG, "stream client gone (recv=%d errno=%d)", r, errno);
                break;
            }
        }
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
        int64_t t_capture = esp_timer_get_time();
        metrics_record_capture(t_capture - t0, fb->len);

        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %u\r\n"
                               "X-Capture-Us: %lld\r\n\r\n",
                               (unsigned)fb->len, (long long)t_capture);
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
