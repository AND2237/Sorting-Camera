/*
 * Raw TCP throughput benchmark (diagnostic build only).
 *
 * Question it answers: how many bits per second can this device push to a PC
 * over its own softAP, with and without the camera running, when the payload
 * comes from internal RAM, from PSRAM, or from real camera frames? The MJPEG
 * stream has only ever been measured at ~4.3-4.7 Mbps (docs/benchmark-results.md),
 * on three different transports, so it is not known whether that is the link or
 * the camera. This isolates the link.
 *
 * Compiled to nothing unless CONFIG_SORTING_CAM_NET_BENCH is set. See
 * Kconfig.projbuild and benchmarks/net_bench.py.
 *
 * Wire protocol (client -> device, one ASCII line):
 *     BENCH <mode> <seconds> <chunk>\n
 *   mode    int | psram | cam
 *   seconds 1..300
 *   chunk   512..32768 bytes per send() call (int: also the buffer size)
 * Device -> client: raw payload for <seconds>, then FIN.
 *   int, psram : unframed bytes.
 *   cam        : repeated { "FRM1", uint32 little-endian length, JPEG bytes }.
 *
 * Second request, usable with no serial connection at all:
 *     INFO\n
 * Device -> client: ONE line of JSON, then FIN. It carries what the serial console
 * would have shown: the sdkconfig the binary was built with, the radio and flash
 * state read back at run time, the reset reason and uptime (a brownout shows as
 * reset_reason 9 / a falling uptime), and the device's own statistics of the last
 * BENCH run. The device also still logs everything on the serial console.
 */
#include "app.h"

#include <stdio.h>

#include "sdkconfig.h"

#ifdef CONFIG_SORTING_CAM_NET_BENCH

#include <errno.h>
#include <lwip/sockets.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "netbench";

#define BENCH_PORT          83
#define BENCH_REQ_MAX       63
#define BENCH_PSRAM_BYTES   (256 * 1024)
#define BENCH_SEND_TIMEOUT_S 5
#define BENCH_SLOW_SEND_US  50000

typedef struct {
    uint64_t bytes;
    uint32_t sends;
    uint32_t slow_sends;   // send() calls that took longer than BENCH_SLOW_SEND_US
    int64_t max_send_us;
    uint32_t frames;
    uint32_t cam_fail;
} bench_stats_t;

// Sends the whole buffer, timing every send() so a stalled TCP window shows up
// as slow_sends / max_send_us instead of being averaged away.
static int send_all(int fd, const uint8_t *buf, size_t len, bench_stats_t *st)
{
    size_t off = 0;
    while (off < len) {
        const int64_t t0 = esp_timer_get_time();
        const int n = send(fd, buf + off, len - off, 0);
        const int64_t dt = esp_timer_get_time() - t0;
        st->sends++;
        if (dt > st->max_send_us) {
            st->max_send_us = dt;
        }
        if (dt > BENCH_SLOW_SEND_US) {
            st->slow_sends++;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1; // includes EAGAIN after SO_SNDTIMEO: the client stopped draining
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
        st->bytes += (uint64_t)n;
    }
    return 0;
}

static void log_build_config(void)
{
    // Proves which sdkconfig this binary was actually built with. sdkconfig.defaults
    // is silently ignored for symbols already present in a stale sdkconfig
    // (ADR-0015), so the measurement must not rely on the defaults file.
#ifdef CONFIG_LWIP_IRAM_OPTIMIZATION
    const int lwip_iram = 1;
#else
    const int lwip_iram = 0;
#endif
#ifdef CONFIG_ESP_WIFI_IRAM_OPT
    const int wifi_iram = 1;
#else
    const int wifi_iram = 0;
#endif
#ifdef CONFIG_ESP_WIFI_RX_IRAM_OPT
    const int wifi_rx_iram = 1;
#else
    const int wifi_rx_iram = 0;
#endif
#ifdef CONFIG_ESPTOOLPY_FLASHMODE_QIO
    const char *flash_mode = "qio";
#else
    const char *flash_mode = "not-qio";
#endif
    ESP_LOGI(TAG,
             "build cfg: cpu=%d MHz flash=%s/%s psram_speed=%d tcp_snd_buf=%d tcp_wnd=%d "
             "wifi_rx_buf=%d wifi_tx_buf=%d wifi_iram=%d wifi_rx_iram=%d lwip_iram=%d",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_ESPTOOLPY_FLASHFREQ, flash_mode,
#ifdef CONFIG_SPIRAM_SPEED
             CONFIG_SPIRAM_SPEED,
#else
             0,
#endif
             CONFIG_LWIP_TCP_SND_BUF_DEFAULT, CONFIG_LWIP_TCP_WND_DEFAULT,
             CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM,
             wifi_iram, wifi_rx_iram, lwip_iram);
}

typedef struct {
    uint32_t runs;          // BENCH runs started since boot (the INFO reply counts this)
    bool finished;
    bool ok;
    char mode[8];
    int secs_req;
    int chunk;
    double secs;
    uint64_t bytes;
    double mbps;
    uint32_t frames;
    double fps;
    uint32_t sends;
    uint32_t slow_sends;
    double max_send_ms;
    uint32_t cam_fail;
    uint32_t free_internal;
    uint32_t min_free_internal;
} bench_last_t;

// Only the single server task reads or writes this, so no lock is needed.
static bench_last_t s_last;

static const char *flash_io_name(void)
{
    if (!esp_flash_default_chip) {
        return "unknown";
    }
    switch (esp_flash_default_chip->read_mode) {
    case SPI_FLASH_QIO:    return "qio";
    case SPI_FLASH_QOUT:   return "qout";
    case SPI_FLASH_DIO:    return "dio";
    case SPI_FLASH_DOUT:   return "dout";
    case SPI_FLASH_FASTRD: return "fastrd";
    case SPI_FLASH_SLOWRD: return "slowrd";
    default:               return "other";
    }
}

// Builds the INFO reply. Everything here is read from the running device or from
// the macros of the sdkconfig this binary was compiled with.
static void send_info(int fd)
{
    static char out[1792];

#ifdef CONFIG_LWIP_IRAM_OPTIMIZATION
    const int lwip_iram = 1;
#else
    const int lwip_iram = 0;
#endif
#ifdef CONFIG_ESP_WIFI_IRAM_OPT
    const int wifi_iram = 1;
#else
    const int wifi_iram = 0;
#endif
#ifdef CONFIG_ESP_WIFI_RX_IRAM_OPT
    const int wifi_rx_iram = 1;
#else
    const int wifi_rx_iram = 0;
#endif
#ifdef CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH
    const int rtos_flash = 1;
#else
    const int rtos_flash = 0;
#endif
#ifdef CONFIG_ESPTOOLPY_FLASHMODE_QIO
    const char *flash_cfg = "qio";
#else
    const char *flash_cfg = "not-qio";
#endif
#ifdef CONFIG_SPIRAM_SPEED
    const int psram_mhz = CONFIG_SPIRAM_SPEED;
#else
    const int psram_mhz = 0;
#endif
#ifdef CONFIG_SORTING_CAM_NET_BENCH_NO_CAMERA
    const int camera_enabled = 0;
#else
    const int camera_enabled = 1;
#endif

    uint8_t proto = 0;
    wifi_bandwidth_t bw = WIFI_BW_HT20;
    int8_t txp = 0;
    uint8_t pri = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_protocol(WIFI_IF_AP, &proto);
    esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);
    esp_wifi_get_max_tx_power(&txp);
    esp_wifi_get_channel(&pri, &sec);

    uint32_t flash_id = 0;
    if (esp_flash_read_id(NULL, &flash_id) != ESP_OK) {
        flash_id = 0;
    }
    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    const bench_last_t *l = &s_last;
    const int n = snprintf(
        out, sizeof(out),
        "{\"fw\":\"netbench\",\"uptime_s\":%lld,\"reset_reason\":%d,"
        "\"camera_enabled\":%d,"
        "\"build\":{\"cpu_mhz\":%d,\"flash_freq\":\"%s\",\"flash_mode_cfg\":\"%s\","
        "\"psram_mhz\":%d,\"tcp_snd_buf\":%d,\"tcp_wnd\":%d,\"wifi_rx_buf\":%d,"
        "\"wifi_tx_buf\":%d,\"wifi_iram\":%d,\"wifi_rx_iram\":%d,\"lwip_iram\":%d,"
        "\"freertos_in_flash\":%d},"
        "\"runtime\":{\"flash_io\":\"%s\",\"flash_id\":\"0x%06x\",\"flash_size\":%u,"
        "\"protocol_bitmap\":%u,\"bandwidth\":\"%s\",\"channel\":%u,"
        "\"max_tx_power_q\":%d,\"free_internal\":%u,\"min_free_internal\":%u,"
        "\"free_spiram\":%u},"
        "\"last\":{\"runs\":%u,\"finished\":%d,\"ok\":%d,\"mode\":\"%s\",\"secs_req\":%d,"
        "\"chunk\":%d,\"secs\":%.3f,\"bytes\":%llu,\"mbps\":%.3f,\"frames\":%u,"
        "\"fps\":%.3f,\"sends\":%u,\"slow_sends\":%u,\"max_send_ms\":%.1f,"
        "\"cam_fail\":%u,\"free_internal\":%u,\"min_free_internal\":%u}}\n",
        (long long)(esp_timer_get_time() / 1000000), (int)esp_reset_reason(), camera_enabled,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_ESPTOOLPY_FLASHFREQ, flash_cfg, psram_mhz,
        CONFIG_LWIP_TCP_SND_BUF_DEFAULT, CONFIG_LWIP_TCP_WND_DEFAULT,
        CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM, wifi_iram,
        wifi_rx_iram, lwip_iram, rtos_flash, flash_io_name(), (unsigned)flash_id,
        (unsigned)flash_size, (unsigned)proto, bw == WIFI_BW_HT40 ? "HT40" : "HT20",
        (unsigned)pri, (int)txp,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)l->runs, l->finished,
        l->ok, l->mode, l->secs_req, l->chunk, l->secs, (unsigned long long)l->bytes, l->mbps,
        (unsigned)l->frames, l->fps, (unsigned)l->sends, (unsigned)l->slow_sends,
        l->max_send_ms, (unsigned)l->cam_fail, (unsigned)l->free_internal,
        (unsigned)l->min_free_internal);
    if (n <= 0 || n >= (int)sizeof(out)) {
        ESP_LOGE(TAG, "INFO reply did not fit (%d bytes)", n);
        return;
    }
    bench_stats_t unused = {0};
    (void)send_all(fd, (const uint8_t *)out, (size_t)n, &unused);
    shutdown(fd, SHUT_WR);
    char sink[16];
    while (recv(fd, sink, sizeof(sink), 0) > 0) {
    }
}

static void serve_client(int fd)
{
    struct timeval rcv_to = {.tv_sec = 3, .tv_usec = 0};
    struct timeval snd_to = {.tv_sec = BENCH_SEND_TIMEOUT_S, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char req[BENCH_REQ_MAX + 1];
    size_t got = 0;
    while (got < BENCH_REQ_MAX) {
        const int n = recv(fd, req + got, 1, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "no request line (recv=%d errno=%d)", n, errno);
            return;
        }
        if (req[got] == '\n') {
            break;
        }
        got++;
    }
    req[got] = '\0';

    if (strncmp(req, "INFO", 4) == 0) {
        send_info(fd);
        return;
    }

    char mode[8] = {0};
    int secs = 0;
    int chunk = 0;
    if (sscanf(req, "BENCH %7s %d %d", mode, &secs, &chunk) != 3) {
        ESP_LOGW(TAG, "bad request '%s'", req);
        return;
    }
    if (secs < 1) {
        secs = 1;
    } else if (secs > 300) {
        secs = 300;
    }
    if (chunk < 512) {
        chunk = 512;
    } else if (chunk > 32768) {
        chunk = 32768;
    }
    const bool m_int = strcmp(mode, "int") == 0;
    const bool m_psram = strcmp(mode, "psram") == 0;
    const bool m_cam = strcmp(mode, "cam") == 0;
    if (!m_int && !m_psram && !m_cam) {
        ESP_LOGW(TAG, "unknown mode '%s'", mode);
        return;
    }

    memset(&s_last.mode, 0, sizeof(s_last.mode));
    s_last.runs++;
    s_last.finished = false;
    s_last.ok = false;
    strncpy(s_last.mode, mode, sizeof(s_last.mode) - 1);
    s_last.secs_req = secs;
    s_last.chunk = chunk;
    s_last.secs = s_last.mbps = s_last.fps = s_last.max_send_ms = 0;
    s_last.bytes = 0;
    s_last.frames = s_last.sends = s_last.slow_sends = s_last.cam_fail = 0;

    log_build_config();
    ESP_LOGI(TAG, "start mode=%s secs=%d chunk=%d rssi=%d free_internal=%u free_spiram=%u", mode,
             secs, chunk, wifi_get_rssi(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint8_t *buf = NULL;
    size_t buf_len = 0;
    if (m_int) {
        buf_len = (size_t)chunk;
        buf = heap_caps_malloc(buf_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    } else if (m_psram) {
        buf_len = BENCH_PSRAM_BYTES;
        buf = heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if ((m_int || m_psram) && !buf) {
        ESP_LOGE(TAG, "could not allocate %u bytes for mode %s", (unsigned)buf_len, mode);
        return;
    }
    if (buf) {
        for (size_t i = 0; i < buf_len; i++) {
            buf[i] = (uint8_t)(i * 31u + 7u);
        }
    }

    bench_stats_t st = {0};
    const int64_t t_start = esp_timer_get_time();
    const int64_t t_end = t_start + (int64_t)secs * 1000000;
    size_t psram_off = 0;
    int rc = 0;

    while (rc == 0 && esp_timer_get_time() < t_end) {
        if (m_int) {
            rc = send_all(fd, buf, buf_len, &st);
        } else if (m_psram) {
            // Walk the buffer so reads miss the 32 KiB external-RAM cache the
            // way a 50-130 KB frame does.
            size_t n = (size_t)chunk;
            if (psram_off + n > buf_len) {
                psram_off = 0;
            }
            rc = send_all(fd, buf + psram_off, n, &st);
            psram_off += n;
        } else {
            camera_fb_t *fb = camera_fb_get();
            if (!fb) {
                st.cam_fail++;
                vTaskDelay(pdMS_TO_TICKS(20));
                if (st.cam_fail > 250) {
                    ESP_LOGE(TAG, "camera produced no frames; aborting");
                    rc = -1;
                }
                continue;
            }
            uint8_t hdr[8] = {'F', 'R', 'M', '1', 0, 0, 0, 0};
            const uint32_t len = (uint32_t)fb->len;
            hdr[4] = (uint8_t)(len & 0xFF);
            hdr[5] = (uint8_t)((len >> 8) & 0xFF);
            hdr[6] = (uint8_t)((len >> 16) & 0xFF);
            hdr[7] = (uint8_t)((len >> 24) & 0xFF);
            rc = send_all(fd, hdr, sizeof(hdr), &st);
            if (rc == 0) {
                rc = send_all(fd, fb->buf, fb->len, &st);
            }
            camera_fb_return(fb);
            if (rc == 0) {
                st.frames++;
            }
        }
    }

    const double secs_real = (double)(esp_timer_get_time() - t_start) / 1e6;
    shutdown(fd, SHUT_WR);
    // Let the peer read what is queued and close; bounded by SO_RCVTIMEO.
    char sink[16];
    while (recv(fd, sink, sizeof(sink), 0) > 0) {
    }
    free(buf);

    s_last.finished = true;
    s_last.ok = (rc == 0);
    s_last.secs = secs_real;
    s_last.bytes = st.bytes;
    s_last.mbps = secs_real > 0 ? (double)st.bytes * 8.0 / secs_real / 1e6 : 0.0;
    s_last.frames = st.frames;
    s_last.fps = secs_real > 0 ? (double)st.frames / secs_real : 0.0;
    s_last.sends = st.sends;
    s_last.slow_sends = st.slow_sends;
    s_last.max_send_ms = (double)st.max_send_us / 1000.0;
    s_last.cam_fail = st.cam_fail;
    s_last.free_internal =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_last.min_free_internal =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "bench done mode=%s result=%s secs=%.2f bytes=%llu device_mbps=%.2f frames=%u "
             "device_fps=%.2f sends=%u slow_sends=%u max_send_ms=%.1f cam_fail=%u "
             "free_internal=%u min_free_internal=%u",
             mode, rc == 0 ? "ok" : "send-failed", secs_real, (unsigned long long)st.bytes,
             secs_real > 0 ? (double)st.bytes * 8.0 / secs_real / 1e6 : 0.0, (unsigned)st.frames,
             secs_real > 0 ? (double)st.frames / secs_real : 0.0, (unsigned)st.sends,
             (unsigned)st.slow_sends, (double)st.max_send_us / 1000.0, (unsigned)st.cam_fail,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void bench_task(void *arg)
{
    (void)arg;
    const int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    const int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(BENCH_PORT);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen on %d failed errno=%d", BENCH_PORT, errno);
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "bench server listening on tcp/%d", BENCH_PORT);
    log_build_config();
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int fd = accept(ls, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        serve_client(fd);
        close(fd);
    }
}

bool net_bench_start(void)
{
    return xTaskCreate(bench_task, "net_bench", 6144, NULL, 5, NULL) == pdPASS;
}

#else /* !CONFIG_SORTING_CAM_NET_BENCH */

bool net_bench_start(void)
{
    return false;
}

#endif
