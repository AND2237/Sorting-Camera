#include "app.h"
#include "auth.h"
#include "camera_control.h"

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
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
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
    cJSON_AddNumberToObject(root, "camera_recoveries", camera_recovery_count());
    cJSON_AddNumberToObject(root, "camera_up", camera_is_up() ? 1 : 0);
    cJSON_AddNumberToObject(root, "frame_budget_bytes", (int)camera_frame_budget());
    cJSON_AddNumberToObject(root, "quality_floor", camera_quality_floor(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "xclk_max_mhz", camera_xclk_max_mhz(camera_current_framesize()));
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
    // Loss and pressure that were previously only in the serial log (FW-19/20).
    cJSON_AddNumberToObject(root, "frames_send_failures", metrics_send_failures());
    cJSON_AddNumberToObject(root, "frames_near_budget", metrics_near_budget_frames());
    cJSON_AddNumberToObject(root, "snapshots_served", metrics_snapshots_served());

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

/*
 * One policy, one message. A write that reconfigures the camera is refused
 * while anybody is watching, and every write endpoint has to go through here:
 * the sensor endpoint used to have no check at all, so a slider could rewrite
 * registers mid-capture while the config endpoint was refusing the same
 * request (FW-12).
 */
static bool refuse_while_streaming(httpd_req_t *req, const char *what)
{
    const int clients = stream_client_count() + frame_transport_client_count();
    if (clients <= 0) {
        return false;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "stream active (%d client(s)): disconnect before %s", clients,
             what);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return true;
}

// The configuration as it now stands. GET returns it to read; POST returns it
// to confirm what the write landed as, so the two handlers cannot drift apart.
static esp_err_t config_state_reply(httpd_req_t *req)
{
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
    cJSON_AddNumberToObject(root, "frame_budget_bytes", (int)camera_frame_budget());
    cJSON_AddNumberToObject(root, "quality_floor", camera_quality_floor(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "xclk_max_mhz", camera_xclk_max_mhz(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "measured_max_frame_bytes",
                            (int)camera_measured_max_frame_bytes(camera_current_framesize()));
    cJSON_AddNumberToObject(root, "camera_recoveries", camera_recovery_count());

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

/*
 * Read-only. Config writes moved to POST (FW-13), so a query string on GET is
 * either a write attempt or a truncated one - and both used to fall through to
 * a 200 that said "applied" about a camera nobody had touched (FW-10). There is
 * no third case worth answering with anything but 400.
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    char query[192];
    const esp_err_t qerr = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (qerr == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config query too long");
        return ESP_FAIL;
    }
    if (qerr == ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "GET /api/v1/config is read-only; send writes as "
                            "POST /api/v1/config with a JSON body");
        return ESP_FAIL;
    }
    return config_state_reply(req);
}

static void config_reject(httpd_req_t *req, const char *msg)
{
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    const int64_t t_config = esp_timer_get_time();

    /*
     * The content type is part of the contract, not decoration (ADR-0017).
     * A browser can issue a *simple* POST - text/plain, no preflight - that
     * executes before any CORS rule gets a chance to refuse it, and this
     * device answers every preflight with 405 because it has no OPTIONS
     * handler. Requiring application/json therefore makes every cross-origin
     * write unsendable, which keeping the wildcard Origin header alone would
     * not.
     */
    char ctype[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK
        || strstr(ctype, "application/json") == NULL) {
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "config writes require Content-Type: application/json",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1-512 bytes of JSON");
        return ESP_FAIL;
    }
    char body[513];
    int received = 0;
    while (received < total) {
        const int chunk = httpd_req_recv(req, body + received, total - received);
        if (chunk <= 0) {
            if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete body");
            return ESP_FAIL;
        }
        received += chunk;
    }
    body[received] = '\0';

    // Checked only once the body is drained: answering before it is read
    // leaves the rest of the request sitting in the socket. A write never
    // carries state in its URL, so there is nothing here to honour - only
    // something to decline out loud.
    char query[192];
    const esp_err_t qerr = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (qerr == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config query too long");
        return ESP_FAIL;
    }
    if (qerr == ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "config writes carry no query string; put every "
                            "setting in the JSON body");
        return ESP_FAIL;
    }

    cJSON *params = cJSON_Parse(body);
    if (!params || !cJSON_IsObject(params)) {
        cJSON_Delete(params);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be a JSON object");
        return ESP_FAIL;
    }

    framesize_t fs = camera_current_framesize();
    int quality = camera_current_quality();
    int xclk = camera_current_xclk_mhz();
    int fb = camera_current_fb_count();
    camera_grab_mode_t grab = camera_current_grab_mode();
    camera_fb_location_t loc = camera_current_fb_location();
    bool any = false;

    const cJSON *item = NULL;
    char msg[160];
    cJSON_ArrayForEach(item, params)
    {
        const char *key = item->string ? item->string : "";
        if (strcmp(key, "framesize") == 0) {
            if (!cJSON_IsString(item) || !parse_framesize(item->valuestring, &fs)) {
                config_reject(req, "bad framesize (qqvga|qvga|vga|svga|xga|hd|sxga|uxga)");
                goto done;
            }
        } else if (strcmp(key, "quality") == 0) {
            if (!cJSON_IsNumber(item)) {
                config_reject(req, "quality must be a number (0-63)");
                goto done;
            }
            quality = item->valueint;
            if (quality < 0 || quality > 63) {
                config_reject(req, "bad quality (0-63)");
                goto done;
            }
        } else if (strcmp(key, "xclk") == 0) {
            if (!cJSON_IsNumber(item)) {
                config_reject(req, "xclk must be a number (6-27 MHz)");
                goto done;
            }
            xclk = item->valueint;
            if (xclk < 6 || xclk > 27) {
                config_reject(req, "bad xclk (6-27 MHz)");
                goto done;
            }
        } else if (strcmp(key, "fb_count") == 0) {
            if (!cJSON_IsNumber(item)) {
                config_reject(req, "fb_count must be a number (1-3)");
                goto done;
            }
            fb = item->valueint;
            if (fb < 1 || fb > 3) {
                config_reject(req, "bad fb_count (1-3)");
                goto done;
            }
        } else if (strcmp(key, "grab") == 0) {
            if (!cJSON_IsString(item)) {
                config_reject(req, "bad grab (latest|cont)");
                goto done;
            }
            if (strcmp(item->valuestring, "latest") == 0) {
                grab = CAMERA_GRAB_LATEST;
            } else if (strcmp(item->valuestring, "cont") == 0) {
                grab = CAMERA_GRAB_WHEN_EMPTY;
            } else {
                config_reject(req, "bad grab (latest|cont)");
                goto done;
            }
        } else if (strcmp(key, "fbloc") == 0) {
            if (!cJSON_IsString(item)) {
                config_reject(req, "bad fbloc (psram|dram)");
                goto done;
            }
            if (strcmp(item->valuestring, "psram") == 0) {
                loc = CAMERA_FB_IN_PSRAM;
            } else if (strcmp(item->valuestring, "dram") == 0) {
                loc = CAMERA_FB_IN_DRAM;
            } else {
                config_reject(req, "bad fbloc (psram|dram)");
                goto done;
            }
        } else {
            // The old query parser dropped what it did not know, which is how a
            // typo became a setting that "worked". A body is explicit; so is a
            // rejection.
            snprintf(msg, sizeof(msg), "unknown config key: %s", key);
            config_reject(req, msg);
            goto done;
        }
        any = true;
    }

    if (any && refuse_while_streaming(req, "config change")) {
        goto done;
    }

    if (any) {
        esp_err_t aerr = camera_apply_config(fs, quality, xclk, fb, grab, loc);
        if (aerr != ESP_OK) {
            if (aerr == ESP_ERR_INVALID_SIZE) {
                snprintf(msg, sizeof(msg),
                         "quality %d is below the measured safe floor %d for %s "
                         "(largest frame measured at that floor is %u B, budget %u B)",
                         quality, camera_quality_floor(fs), framesize_name(fs),
                         (unsigned)camera_measured_max_frame_bytes(fs),
                         (unsigned)camera_frame_budget());
            } else if (aerr == ESP_ERR_INVALID_ARG && xclk > camera_xclk_max_mhz(fs)) {
                snprintf(msg, sizeof(msg),
                         "xclk %d MHz is above the measured ceiling %d MHz for %s "
                         "(higher clocks produce no frames at this resolution)",
                         xclk, camera_xclk_max_mhz(fs), framesize_name(fs));
            } else {
                snprintf(msg, sizeof(msg), "camera apply failed: %s", esp_err_to_name(aerr));
            }
            config_reject(req, msg);
            goto done;
        }
        ESP_LOGI(TAG, "config applied in %lld ms",
                 (long long)((esp_timer_get_time() - t_config) / 1000));
    }

    cJSON_Delete(params);
    return config_state_reply(req);

done:
    cJSON_Delete(params);
    return ESP_FAIL;
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    camera_fb_t *fb = camera_fb_get();
    if (!fb) {
        metrics_capture_failure();
        camera_recover();
        fb = camera_fb_get();
        if (!fb) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_fb_return(fb);
    // Counted in its own bucket on purpose: a snapshot does not touch
    // frames_captured/frames_delivered, so a client polling /snapshot can never
    // make a dead stream look alive to the liveness watchdog (FW-19).
    if (err == ESP_OK) {
        metrics_record_snapshot();
    }
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    static const char boundary[] = "--FRAME\r\n";
    static const char boundary_cont[] = "\r\n--FRAME\r\n";
    // RFC 2046 close-delimiter: ends the multipart body properly instead of
    // leaving the reader between two boundaries (FW-9).
    static const char close_delim[] = "\r\n--FRAME--\r\n";
    char part_hdr[128];
    bool first = true;
    int consecutive_fails = 0;
    const int sockfd = httpd_req_to_sockfd(req);

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=FRAME");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    atomic_fetch_add(&s_stream_clients, 1);
    metrics_mark_stream_active();

    int recovery_attempts = 0;
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
            if (consecutive_fails >= STREAM_CAPTURE_FAIL_LIMIT) {
                consecutive_fails = 0;
                if (recovery_attempts < 2) {
                    recovery_attempts++;
                    ESP_LOGE(TAG, "capture failing, recovering camera (attempt %d)",
                             recovery_attempts);
                    camera_recover();
                    continue;
                }
                ESP_LOGE(TAG, "stream: capture unrecoverable, closing client");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        consecutive_fails = 0;
        recovery_attempts = 0;
        int64_t t_capture = esp_timer_get_time();
        metrics_record_capture(t_capture - t0, fb->len);

        const size_t budget = camera_frame_budget();
        if (budget && fb->len * 10 >= budget * 9) {
            metrics_record_near_budget();
            const uint32_t n = metrics_near_budget_frames();
            if (n == 1 || n % 100 == 0) {
                ESP_LOGW(TAG, "frame %u B is >=90%% of the %u B budget - lower the quality",
                         (unsigned)fb->len, (unsigned)budget);
            }
        }

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
            // The frame that died with the connection is otherwise invisible:
            // the client's counters never see it and ours would have counted
            // neither capture nor delivery for it (FW-19).
            metrics_record_send_failure();
            break;
        }
        metrics_record_delivery();
    }

    // RFC 2046 close-delimiter for the multipart body. Without it the response
    // simply stops between two boundaries and any conforming reader sits in
    // "waiting for a boundary" forever (FW-9). It is sent unconditionally: by
    // now every error path has already decided to leave, and the best thing a
    // well-behaved client can do with a failed terminator is close the socket.
    httpd_resp_send_chunk(req, close_delim, sizeof(close_delim) - 1);
    atomic_fetch_sub(&s_stream_clients, 1);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

int stream_client_count(void)
{
    return atomic_load(&s_stream_clients);
}

static const struct {
    const char *key;
    framesize_t fs;
    int width;
    int height;
} s_resolutions[] = {
    {"qqvga", FRAMESIZE_QQVGA, 160, 120},
    {"qvga", FRAMESIZE_QVGA, 320, 240},
    {"vga", FRAMESIZE_VGA, 640, 480},
    {"svga", FRAMESIZE_SVGA, 800, 600},
    {"xga", FRAMESIZE_XGA, 1024, 768},
    {"hd", FRAMESIZE_HD, 1280, 720},
    {"sxga", FRAMESIZE_SXGA, 1280, 1024},
    {"uxga", FRAMESIZE_UXGA, 1600, 1200},
};

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
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

static cJSON *resolutions_json(void)
{
    cJSON *res = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(s_resolutions) / sizeof(s_resolutions[0]); i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "key", s_resolutions[i].key);
        cJSON_AddNumberToObject(r, "width", s_resolutions[i].width);
        cJSON_AddNumberToObject(r, "height", s_resolutions[i].height);
        cJSON_AddNumberToObject(r, "quality_floor", camera_quality_floor(s_resolutions[i].fs));
        cJSON_AddNumberToObject(r, "xclk_max_mhz", camera_xclk_max_mhz(s_resolutions[i].fs));
        cJSON_AddItemToArray(res, r);
    }
    return res;
}

static esp_err_t capabilities_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "fw_version", app ? app->version : FW_VERSION);
    cJSON_AddNumberToObject(root, "proto_version", PROTO_VERSION);
    cJSON_AddStringToObject(root, "device_name", DEVICE_NAME);
    cJSON_AddStringToObject(root, "device_id", device_id_hex());
    cJSON_AddStringToObject(root, "ip", device_ip());
    cJSON_AddStringToObject(root, "sensor", "ov2640");
    cJSON_AddNumberToObject(root, "control_port", CONTROL_HTTP_PORT);
    cJSON_AddNumberToObject(root, "stream_port", STREAM_HTTP_PORT);
    cJSON_AddNumberToObject(root, "discovery_port", DISCOVERY_UDP_PORT);
    cJSON_AddNumberToObject(root, "frame_budget_bytes", (int)camera_frame_budget());
    cJSON_AddItemToObject(root, "resolutions", resolutions_json());
    camera_control_add_capabilities(root);
    auth_add_capabilities(root);
    return send_json(req, root);
}

static esp_err_t sensor_get_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    camera_control_add_status(root);
    return send_json(req, root);
}

static esp_err_t sensor_set_handler(httpd_req_t *req)
{
    if (!auth_authorized(req)) {
        return ESP_FAIL;
    }
    const int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1-512 bytes of JSON");
        return ESP_FAIL;
    }

    char body[513];
    int received = 0;
    while (received < total) {
        const int chunk = httpd_req_recv(req, body + received, total - received);
        if (chunk <= 0) {
            if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete body");
            return ESP_FAIL;
        }
        received += chunk;
    }
    body[received] = '\0';

    // The body is read before the policy check so the connection is drained
    // first, and the sensor endpoint is then held to exactly the rule the
    // config endpoint has always followed: nobody may reconfigure the camera
    // while somebody is watching it (FW-12).
    if (refuse_while_streaming(req, "sensor change")) {
        return ESP_FAIL;
    }

    cJSON *params = cJSON_Parse(body);
    if (!params) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
        return ESP_FAIL;
    }

    char err[160] = {0};
    // Excludes the SCCB writes from a concurrent camera_recover() /
    // camera_apply_config() teardown-reinit window; those hold the same mutex
    // for their whole run (FW-12).
    if (!camera_lock()) {
        cJSON_Delete(params);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "camera subsystem is not running");
        return ESP_FAIL;
    }
    esp_err_t aerr = camera_control_apply(params, err, sizeof(err));
    camera_unlock();
    cJSON_Delete(params);
    if (aerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err[0] ? err : "apply failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    camera_control_add_status(root);
    return send_json(req, root);
}

static esp_err_t send_status_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_challenge_handler(httpd_req_t *req)
{
    if (!auth_is_enabled()) {
        return send_status_text(req, "409 Conflict",
                                "authentication not provisioned on this device");
    }
    if (auth_is_locked_out(req)) {
        return send_status_text(req, "429 Too Many Requests", "client is in backoff");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    if (!auth_issue_challenge(req, root)) {
        cJSON_Delete(root);
        return send_status_text(req, "429 Too Many Requests", "client is in backoff");
    }
    return send_json(req, root);
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    const int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1-256 bytes of JSON");
        return ESP_FAIL;
    }
    char body[257];
    int received = 0;
    while (received < total) {
        const int chunk = httpd_req_recv(req, body + received, total - received);
        if (chunk <= 0) {
            if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete body");
            return ESP_FAIL;
        }
        received += chunk;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
        return ESP_FAIL;
    }
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(root, "nonce");
    const cJSON *proof = cJSON_GetObjectItemCaseSensitive(root, "proof");
    char token[64] = {0};
    char err[96] = {0};
    const bool ok = auth_verify_login(req, cJSON_IsString(nonce) ? nonce->valuestring : NULL,
                                      cJSON_IsString(proof) ? proof->valuestring : NULL, token,
                                      sizeof(token), err, sizeof(err));
    cJSON_Delete(root);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, err[0] ? err : "login failed");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddNumberToObject(resp, "expires_in_s", 1800);
    cJSON_AddStringToObject(resp, "device_id", device_id_hex());
    return send_json(req, resp);
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    auth_logout(req);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "logged_out", true);
    return send_json(req, root);
}

esp_err_t start_control_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONTROL_HTTP_PORT;
    cfg.ctrl_port = 32768;
    cfg.max_open_sockets = 6;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 2;
    cfg.stack_size = 8192;

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
    const httpd_uri_t config_get = {
        .uri = "/api/v1/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    // Same URI, different method: esp_http_server dispatches on URI *and*
    // method, and a GET carrying a write is refused by the handler rather
    // than being matched to this one (ADR-0017).
    const httpd_uri_t config_post = {
        .uri = "/api/v1/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    const httpd_uri_t capabilities = {
        .uri = "/api/v1/capabilities",
        .method = HTTP_GET,
        .handler = capabilities_handler,
    };
    const httpd_uri_t sensor_get = {
        .uri = "/api/v1/sensor",
        .method = HTTP_GET,
        .handler = sensor_get_handler,
    };
    const httpd_uri_t sensor_set = {
        .uri = "/api/v1/sensor",
        .method = HTTP_POST,
        .handler = sensor_set_handler,
    };
    const httpd_uri_t auth_challenge = {
        .uri = "/api/v1/auth/challenge",
        .method = HTTP_GET,
        .handler = auth_challenge_handler,
    };
    const httpd_uri_t auth_login = {
        .uri = "/api/v1/auth/login",
        .method = HTTP_POST,
        .handler = auth_login_handler,
    };
    const httpd_uri_t auth_logout = {
        .uri = "/api/v1/auth/logout",
        .method = HTTP_POST,
        .handler = auth_logout_handler,
    };
    httpd_register_uri_handler(s_control_server, &status);
    httpd_register_uri_handler(s_control_server, &snapshot);
    httpd_register_uri_handler(s_control_server, &config_get);
    httpd_register_uri_handler(s_control_server, &config_post);
    httpd_register_uri_handler(s_control_server, &capabilities);
    httpd_register_uri_handler(s_control_server, &sensor_get);
    httpd_register_uri_handler(s_control_server, &sensor_set);
    httpd_register_uri_handler(s_control_server, &auth_challenge);
    httpd_register_uri_handler(s_control_server, &auth_login);
    httpd_register_uri_handler(s_control_server, &auth_logout);
    ESP_LOGI(TAG, "control server on port %d (auth %s)", CONTROL_HTTP_PORT,
             auth_is_enabled() ? "required" : "not provisioned");
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
