#include "app.h"

#include <errno.h>
#include <lwip/sockets.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xport";

#define FRAME_PROTO_VERSION  1
#define FRAME_HEADER_LEN     36
#define UDP_DGRAM_HEADER_LEN 16
#define UDP_MTU              1400
#define UDP_PAYLOAD_MAX      (UDP_MTU - UDP_DGRAM_HEADER_LEN)
#define UDP_PEER_TIMEOUT_MS  3000
#define TX_TIMEOUT_S         5

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t header_len;
    uint16_t device_id;
    uint32_t seq;
    uint64_t timestamp_us;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
    uint8_t compression;
    uint32_t payload_len;
    uint16_t flags;
    uint32_t crc32;
} frame_header_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t seq;
    uint16_t frag_index;
    uint16_t frag_count;
    uint32_t frame_crc32;
} udp_header_t;

_Static_assert(sizeof(frame_header_t) == FRAME_HEADER_LEN, "frame header must be 36 bytes");
_Static_assert(sizeof(udp_header_t) == UDP_DGRAM_HEADER_LEN, "udp header must be 16 bytes");

static uint16_t s_device_id;
static volatile int s_tcp_clients;
static volatile int s_udp_peer;
static int64_t s_udp_peer_deadline;
static struct sockaddr_in s_udp_peer_addr;
static volatile uint32_t s_udp_tx_dgrams;
static volatile uint32_t s_udp_tx_drops;

static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    return esp_rom_crc32_le(0, data, len);
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        int n = send(fd, p + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void fill_frame_header(frame_header_t *h, camera_fb_t *fb, uint32_t seq,
                              int64_t timestamp_us)
{
    memcpy(h->magic, "SCAM", 4);
    h->version = FRAME_PROTO_VERSION;
    h->header_len = FRAME_HEADER_LEN;
    h->device_id = s_device_id;
    h->seq = seq;
    h->timestamp_us = (uint64_t)timestamp_us;
    h->width = fb->width;
    h->height = fb->height;
    h->pixel_format = 0;
    h->compression = 0;
    h->payload_len = (uint32_t)fb->len;
    h->flags = 1;
    h->crc32 = crc32_buf(fb->buf, fb->len);
}

static void tcp_task(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "tcp socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(FRAME_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
        || listen(listen_fd, 1) < 0) {
        ESP_LOGE(TAG, "tcp bind/listen failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "tcp framed server on port %d", FRAME_TCP_PORT);

    uint32_t seq = 0;
    while (true) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct timeval tv = { .tv_sec = TX_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        s_tcp_clients = 1;
        metrics_mark_stream_active();
        ESP_LOGI(TAG, "tcp client connected");
        seq = 0;
        int fails = 0;
        int recovery_attempts = 0;

        while (true) {
            int64_t t0 = esp_timer_get_time();
            camera_fb_t *fb = camera_fb_get();
            if (!fb) {
                metrics_capture_failure();
                // FW-4: this loop tolerated 50 consecutive failures and never
                // recovered the camera - ADR-0009's root cause, kept alive
                // here while the HTTP handler was fixed. The policy is now the
                // same one the MJPEG handler uses: STREAM_CAPTURE_FAIL_LIMIT
                // (1) failures trigger a recovery, two recoveries per
                // connection, then close.
                if (++fails >= STREAM_CAPTURE_FAIL_LIMIT) {
                    fails = 0;
                    if (recovery_attempts < 2) {
                        recovery_attempts++;
                        ESP_LOGE(TAG, "tcp: capture failing, recovering camera (attempt %d)",
                                 recovery_attempts);
                        camera_recover();
                        continue;
                    }
                    ESP_LOGE(TAG, "tcp: capture unrecoverable, closing client");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            fails = 0;
            recovery_attempts = 0;
            int64_t t_capture = esp_timer_get_time();
            metrics_record_capture(t_capture - t0, fb->len);

            frame_header_t h;
            fill_frame_header(&h, fb, seq++, t_capture);

            int err = send_all(fd, &h, sizeof(h));
            if (err == 0) {
                err = send_all(fd, fb->buf, fb->len);
            }
            camera_fb_return(fb);
            if (err != 0) {
                ESP_LOGI(TAG, "tcp client disconnected (errno %d)", errno);
                // The frame started and died with the connection. captured is
                // already incremented while delivered is not, but nothing said
                // *why* - every drop has to be counted, not just inferable
                // (FW-19).
                metrics_record_send_failure();
                break;
            }
            metrics_record_delivery();
        }

        close(fd);
        s_tcp_clients = 0;
    }
}

static void udp_task(void *arg)
{
    (void)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "udp socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(FRAME_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "udp bind failed: errno %d", errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "udp packetized server on port %d", FRAME_UDP_PORT);

    uint8_t pkt[UDP_MTU];
    uint32_t seq = 0;
    bool peer_was_active = false;
    int udp_fails = 0;
    int udp_recovery_attempts = 0;

    while (true) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        int r = select(fd + 1, &rd, NULL, NULL, &tv);

        if (r > 0 && FD_ISSET(fd, &rd)) {
            uint8_t ctl[32];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(fd, ctl, sizeof(ctl), 0, (struct sockaddr *)&from, &from_len);
            if (n >= 8 && memcmp(ctl, "SCSTART1", 8) == 0) {
                s_udp_peer_addr = from;
                s_udp_peer = 1;
                s_udp_peer_deadline = esp_timer_get_time()
                                      + (int64_t)UDP_PEER_TIMEOUT_MS * 1000;
                if (!peer_was_active) {
                    metrics_mark_stream_active();
                    ESP_LOGI(TAG, "udp peer active");
                }
                peer_was_active = true;
            } else if (n >= 14 && memcmp(ctl, "SCSYNC", 6) == 0) {
                uint64_t t1;
                memcpy(&t1, ctl + 6, sizeof(t1));
                uint64_t t2 = (uint64_t)esp_timer_get_time();
                uint8_t reply[21];
                memcpy(reply, "SCSYR", 5);
                memcpy(reply + 5, &t1, sizeof(t1));
                memcpy(reply + 13, &t2, sizeof(t2));
                sendto(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, from_len);
            }
        }

        if (!s_udp_peer) {
            continue;
        }
        if (esp_timer_get_time() > s_udp_peer_deadline) {
            s_udp_peer = 0;
            peer_was_active = false;
            ESP_LOGI(TAG, "udp peer timed out");
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = camera_fb_get();
        if (!fb) {
            metrics_capture_failure();
            // FW-4: the UDP task had no failure limit and no recovery at all -
            // it spun on a dead camera while holding the peer as active. Same
            // bound as the TCP and HTTP paths; with no connection to close it
            // drops the peer instead, so the client has to handshake again.
            if (++udp_fails >= STREAM_CAPTURE_FAIL_LIMIT) {
                udp_fails = 0;
                if (udp_recovery_attempts < 2) {
                    udp_recovery_attempts++;
                    ESP_LOGE(TAG, "udp: capture failing, recovering camera (attempt %d)",
                             udp_recovery_attempts);
                    camera_recover();
                    continue;
                }
                ESP_LOGE(TAG, "udp: capture unrecoverable, dropping peer");
                s_udp_peer = 0;
                peer_was_active = false;
                udp_recovery_attempts = 0;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        udp_fails = 0;
        udp_recovery_attempts = 0;
        int64_t t_capture = esp_timer_get_time();
        metrics_record_capture(t_capture - t0, fb->len);

        uint32_t frame_crc = crc32_buf(fb->buf, fb->len);
        uint16_t frag_count =
            (uint16_t)((fb->len + UDP_PAYLOAD_MAX - 1) / UDP_PAYLOAD_MAX);
        bool frame_ok = true;
        uint32_t sent_frags = 0;

        for (uint16_t i = 0; i < frag_count; i++) {
            size_t off = (size_t)i * UDP_PAYLOAD_MAX;
            size_t chunk = fb->len - off;
            if (chunk > UDP_PAYLOAD_MAX) {
                chunk = UDP_PAYLOAD_MAX;
            }
            udp_header_t uh;
            memcpy(uh.magic, "SCU1", 4);
            uh.seq = seq;
            uh.frag_index = i;
            uh.frag_count = frag_count;
            uh.frame_crc32 = frame_crc;
            memcpy(pkt, &uh, sizeof(uh));
            memcpy(pkt + sizeof(uh), fb->buf + off, chunk);

            int n = sendto(fd, pkt, sizeof(uh) + chunk, 0,
                           (struct sockaddr *)&s_udp_peer_addr,
                           sizeof(s_udp_peer_addr));
            if (n < 0) {
                s_udp_tx_drops++;
                frame_ok = false;
                break;
            }
            sent_frags++;
        }
        s_udp_tx_dgrams += sent_frags;
        camera_fb_return(fb);

        if (frame_ok) {
            seq++;
            metrics_record_delivery();
        } else {
            seq++;
        }
    }
}

bool frame_transport_start(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read MAC for device id");
        return false;
    }
    s_device_id = (uint16_t)((mac[4] << 8) | mac[5]);

    if (xTaskCreate(tcp_task, "frame_tcp", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "tcp task creation failed");
        return false;
    }
    if (xTaskCreate(udp_task, "frame_udp", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "udp task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "frame transport started (device_id=0x%04x)", s_device_id);
    return true;
}

int frame_transport_client_count(void)
{
    return s_tcp_clients + (s_udp_peer ? 1 : 0);
}

int frame_transport_tcp_clients(void)
{
    return s_tcp_clients;
}

int frame_transport_udp_peer(void)
{
    return s_udp_peer ? 1 : 0;
}

uint32_t frame_transport_udp_tx_dgrams(void)
{
    return s_udp_tx_dgrams;
}

uint32_t frame_transport_udp_tx_drops(void)
{
    return s_udp_tx_drops;
}
