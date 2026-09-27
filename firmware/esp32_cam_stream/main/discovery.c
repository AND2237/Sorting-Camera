#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "auth.h"
#include "camera_control.h"

static const char *TAG = "discovery";

#define DISCOVERY_MAX_PACKET 512
#define DISCOVERY_STACK     4096
#define DISCOVERY_REPLY_MIN_MS 250

static const char *s_framesize_key(framesize_t fs)
{
    switch (fs) {
    case FRAMESIZE_UXGA: return "uxga";
    case FRAMESIZE_SXGA: return "sxga";
    case FRAMESIZE_HD: return "hd";
    case FRAMESIZE_XGA: return "xga";
    case FRAMESIZE_SVGA: return "svga";
    case FRAMESIZE_VGA: return "vga";
    case FRAMESIZE_QVGA: return "qvga";
    case FRAMESIZE_QQVGA: return "qqvga";
    default: return NULL;
    }
}

static char *build_announce(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "scam", 1);
    cJSON_AddStringToObject(root, "op", "announce");
    cJSON_AddStringToObject(root, "device_id", device_id_hex());
    cJSON_AddStringToObject(root, "device_name", DEVICE_NAME);
    cJSON_AddStringToObject(root, "ip", device_ip());
    cJSON_AddStringToObject(root, "fw_version", app ? app->version : FW_VERSION);
    cJSON_AddNumberToObject(root, "proto_version", PROTO_VERSION);
    cJSON_AddStringToObject(root, "sensor", "ov2640");
    cJSON_AddNumberToObject(root, "control_port", CONTROL_HTTP_PORT);
    cJSON_AddNumberToObject(root, "stream_port", STREAM_HTTP_PORT);
    cJSON_AddBoolToObject(root, "auth_required", auth_is_enabled());

    cJSON *res = cJSON_AddArrayToObject(root, "resolutions");
    for (int i = 0; i <= FRAMESIZE_UXGA; i++) {
        const char *key = s_framesize_key((framesize_t)i);
        if (!key) {
            continue;
        }
        cJSON_AddItemToArray(res, cJSON_CreateString(key));
    }

    cJSON *ctrl = cJSON_AddObjectToObject(root, "controls");
    for (int i = 0; i < camera_control_count(); i++) {
        const cam_ctrl_def_t *def = camera_control_def(i);
        if (!def || !def->supported) {
            continue;
        }
        cJSON *group = cJSON_GetObjectItemCaseSensitive(ctrl, def->group);
        if (!group) {
            group = cJSON_AddArrayToObject(ctrl, def->group);
        }
        if (group) {
            cJSON_AddItemToArray(group, cJSON_CreateString(def->name));
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static void discovery_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return;
    }
    const int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed: errno %d", DISCOVERY_UDP_PORT, errno);
        close(sock);
        return;
    }
    ESP_LOGI(TAG, "discovery responder on UDP %d", DISCOVERY_UDP_PORT);

    int64_t last_reply_us = 0;

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        const int ready = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) {
            continue;
        }

        char buf[DISCOVERY_MAX_PACKET];
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int received = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                      (struct sockaddr *)&peer, &peer_len);
        if (received <= 0) {
            continue;
        }
        buf[received] = '\0';

        cJSON *query = cJSON_Parse(buf);
        if (!query) {
            continue;
        }
        const cJSON *magic = cJSON_GetObjectItemCaseSensitive(query, "scam");
        const cJSON *op = cJSON_GetObjectItemCaseSensitive(query, "op");
        const bool is_discover = cJSON_IsNumber(magic) && magic->valuedouble == 1 &&
                                 cJSON_IsString(op) && strcmp(op->valuestring, "discover") == 0;
        cJSON_Delete(query);
        if (!is_discover) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_reply_us < DISCOVERY_REPLY_MIN_MS * 1000) {
            continue;
        }
        last_reply_us = now;

        char *reply = build_announce();
        if (!reply) {
            continue;
        }
        sendto(sock, reply, strlen(reply), 0, (struct sockaddr *)&peer, peer_len);
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "answered discovery from %s (%u bytes)", ip, (unsigned)strlen(reply));
        cJSON_free(reply);
    }
}

esp_err_t discovery_start(void)
{
    if (xTaskCreate(discovery_task, "discovery", DISCOVERY_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "discovery task creation failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}
