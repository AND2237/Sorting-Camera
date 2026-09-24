#include "app.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "metrics";

static uint32_t s_captured;
static uint32_t s_delivered;
static uint32_t s_capture_failures;
static int64_t s_avg_capture_us;
static size_t s_last_bytes;
static int64_t s_last_delivery_us;
static SemaphoreHandle_t s_mutex;

static void ensure_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void metrics_record_capture(int64_t duration_us, size_t bytes)
{
    ensure_mutex();
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_captured++;
    s_last_bytes = bytes;
    if (s_avg_capture_us == 0) {
        s_avg_capture_us = duration_us;
    } else {
        s_avg_capture_us = (s_avg_capture_us * 7 + duration_us) / 8;
    }
    xSemaphoreGive(s_mutex);
}

void metrics_record_delivery(void)
{
    ensure_mutex();
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delivered++;
    s_last_delivery_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

void metrics_capture_failure(void)
{
    ensure_mutex();
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_failures++;
    xSemaphoreGive(s_mutex);
}

void metrics_mark_stream_active(void)
{
    ensure_mutex();
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_last_delivery_us == 0) {
        s_last_delivery_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_mutex);
}

int64_t metrics_us_since_last_delivery(void)
{
    ensure_mutex();
    if (!s_mutex) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t last = s_last_delivery_us;
    xSemaphoreGive(s_mutex);
    if (last == 0) {
        return 0;
    }
    return esp_timer_get_time() - last;
}

uint32_t metrics_frames_captured(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_captured;
    xSemaphoreGive(s_mutex);
    return v;
}

uint32_t metrics_frames_delivered(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_delivered;
    xSemaphoreGive(s_mutex);
    return v;
}

uint32_t metrics_capture_failures(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t v = s_capture_failures;
    xSemaphoreGive(s_mutex);
    return v;
}

int64_t metrics_avg_capture_us(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t v = s_avg_capture_us;
    xSemaphoreGive(s_mutex);
    return v;
}

size_t metrics_last_frame_bytes(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t v = s_last_bytes;
    xSemaphoreGive(s_mutex);
    return v;
}

static void metrics_log_task(void *arg)
{
    (void)arg;
    uint32_t last_captured = 0;
    uint32_t last_delivered = 0;
    int64_t last_log = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t cap = metrics_frames_captured();
        uint32_t del = metrics_frames_delivered();
        int64_t now = esp_timer_get_time();
        double secs = (now - last_log) / 1e6;
        if (secs <= 0) {
            secs = 1;
        }
        ESP_LOGI(TAG,
                 "capture_fps=%.1f delivery_fps=%.1f captured=%u delivered=%u fail=%u "
                 "avg_capture_ms=%.1f last_bytes=%u heap=%u",
                 (cap - last_captured) / secs, (del - last_delivered) / secs,
                 (unsigned)cap, (unsigned)del, (unsigned)metrics_capture_failures(),
                 metrics_avg_capture_us() / 1000.0, (unsigned)metrics_last_frame_bytes(),
                 (unsigned)esp_get_free_heap_size());
        last_captured = cap;
        last_delivered = del;
        last_log = now;
    }
}

void metrics_start(void)
{
    ensure_mutex();
    xTaskCreate(metrics_log_task, "metrics_log", 4096, NULL, 1, NULL);
}
