#include "app.h"
#include "camera_control.h"

#include <string.h>

#include "camera_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "camera";

static const char *NVS_NS = "camcfg";
static const char *NVS_KEY = "v1";

typedef struct {
    uint32_t magic;
    uint16_t framesize;
    uint16_t quality;
    uint16_t fb_count;
    uint16_t xclk_mhz;
    uint8_t grab_mode;
    uint8_t fb_location;
    uint8_t version;
    uint8_t reserved;
    uint32_t crc;
} cam_cfg_nvs_t;

#define CAM_CFG_NVS_MAGIC 0x43414D31u
#define CAM_CFG_NVS_VERSION 1

static void camera_sensor_power_cycle(void)
{
    if (CAM_PIN_PWDN < 0) {
        return;
    }
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << CAM_PIN_PWDN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_set_level(CAM_PIN_PWDN, 1);
    vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(CAM_PIN_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void camera_i2c_bus_recovery(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << CAM_PIN_SIOC) | (1ULL << CAM_PIN_SIOD),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_set_level(CAM_PIN_SIOD, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    for (int i = 0; i < 9 && gpio_get_level(CAM_PIN_SIOD) == 0; i++) {
        gpio_set_level(CAM_PIN_SIOC, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(CAM_PIN_SIOC, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    gpio_set_level(CAM_PIN_SIOD, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(CAM_PIN_SIOC, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(CAM_PIN_SIOD, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
}

// How long a teardown may wait for frame buffers to come back before it gives
// up. A consumer holds a buffer for at most its socket send timeout - 5 s on
// every path (TX_TIMEOUT_S in frame_transport.c, httpd's default
// send_wait_timeout on the HTTP servers) - and esp_camera_fb_get() may sit
// inside the driver for up to 4 s. Both run concurrently, so this is a little
// more than the send timeout alone rather than the sum.
#define CAM_TEARDOWN_TIMEOUT_MS 7000
// The gate is polled rather than signalled, because FreeRTOS semaphores have
// no broadcast: several consumers can be waiting for it at once and a binary
// semaphore would wake exactly one of them.
#define CAM_GATE_POLL_MS 10

static SemaphoreHandle_t s_cam_mutex;
static bool s_driver_up;
static uint32_t s_recovery_count;
static camera_fb_t *s_last_fb;
// Guarded by s_cam_mutex. s_gate_closed is set while the driver is being torn
// down or rebuilt: no frame buffer leaves the driver and no esp_camera_fb_get()
// call sits inside it until the gate is open again. The two counters are what
// let a teardown wait for work it cannot simply block - s_gets_in_flight for a
// call already inside the driver, s_fb_held for buffers out with consumers.
static bool s_gate_closed;
static int s_gets_in_flight;
static int s_fb_held;
static int s_fb_count = CAM_DEFAULT_FB_COUNT;
static int s_quality = CAM_DEFAULT_JPEG_QUALITY;
static int s_xclk_mhz = CAM_DEFAULT_XCLK_HZ / 1000000;
static framesize_t s_framesize = CAM_DEFAULT_FRAMESIZE;
static camera_grab_mode_t s_grab_mode = CAM_DEFAULT_GRAB_MODE;
static camera_fb_location_t s_fb_location = CAM_DEFAULT_FB_LOCATION;

/*
 * Frame budget. The esp32-camera JPEG path ignores camera_config_t.fb_size and
 * sizes every buffer from CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE (AUTO builds use
 * width*height/5, which measured 71-99% full at quality 4 and overflowed below
 * that, wedging the OV2640 in a permanent cam_hal FB-OVF state). We therefore
 * build with an explicit budget and refuse any framesize/quality combination
 * whose estimated worst-case frame cannot fit.
 */
#define CAM_FRAME_BUDGET_BYTES ((size_t)CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE)
#define CAM_FRAME_SAFETY_NUM 5u
#define CAM_FRAME_SAFETY_DEN 4u

typedef struct {
    framesize_t fs;
    uint16_t width;
    uint16_t height;
    int safe_quality;
    uint32_t measured_max_bytes;
} cam_size_model_t;

/*
 * safe_quality is the lowest JPEG quality measured to fit the frame budget with
 * 25% margin on this hardware; measured_max_bytes is the largest frame observed
 * at that quality over a 10 s stream (benchmarks/results/qfloor-20260926.json,
 * 44-144 frames per cell). Frames grow steeply below these points and the
 * maximum is scene-dependent - repeated verification runs saw up to ~2x the
 * 10 s maximum for the same cell (uxga/q12: 128,659 B measured, 259,471 B
 * later) - so a linear extrapolation is unsafe and is not used, and floors that
 * showed <2x margin in verification are raised accordingly.
 */
static const cam_size_model_t s_size_models[] = {
    {FRAMESIZE_QQVGA, 160, 120, 0, 20000},
    {FRAMESIZE_QVGA, 320, 240, 0, 65544},
    {FRAMESIZE_VGA, 640, 480, 2, 113239},
    {FRAMESIZE_SVGA, 800, 600, 2, 163923},
    {FRAMESIZE_XGA, 1024, 768, 6, 152548},
    {FRAMESIZE_HD, 1280, 720, 6, 165163},
    {FRAMESIZE_SXGA, 1280, 1024, 8, 201317},
    {FRAMESIZE_UXGA, 1600, 1200, 16, 98587},
};

static const cam_size_model_t *camera_size_model(framesize_t fs)
{
    for (size_t i = 0; i < sizeof(s_size_models) / sizeof(s_size_models[0]); i++) {
        if (s_size_models[i].fs == fs) {
            return &s_size_models[i];
        }
    }
    return NULL;
}

size_t camera_frame_budget(void)
{
    return CAM_FRAME_BUDGET_BYTES;
}

uint32_t camera_estimate_frame_bytes(framesize_t fs, int quality)
{
    const cam_size_model_t *m = camera_size_model(fs);
    if (!m) {
        return 0;
    }
    (void)quality;
    return m->measured_max_bytes;
}

int camera_quality_floor(framesize_t fs)
{
    const cam_size_model_t *m = camera_size_model(fs);
    return m ? m->safe_quality : 0;
}

/*
 * Measured XCLK ceiling per resolution. The OV2640 does not tolerate a clock
 * that is too high for the mode: at 1280x720, 24 MHz and above produced no
 * frames at all and the recovery path could not revive it, while 22 MHz inflated
 * frames from 58 KB to 246 KB and dropped the rate to 2.2 fps. Only combinations
 * that were actually measured are listed - everything else keeps the full
 * accepted range rather than a guessed one (2026-09-27, 120 s runs each).
 */
int camera_xclk_max_mhz(framesize_t fs)
{
    switch (fs) {
    case FRAMESIZE_HD:
        return 20;
    default:
        return 27;
    }
}

static esp_err_t camera_driver_init(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = (uint32_t)s_xclk_mhz * 1000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = s_framesize,
        .jpeg_quality = s_quality,
        .fb_count = s_fb_count,
        .fb_location = s_fb_location,
        .grab_mode = s_grab_mode,
    };

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "camera init in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        // esp_camera_init always starts the sensor at these. They are the
        // baseline, not the answer: CP-4 / FW-1 found that they were the only
        // thing that ever ran after a camera_recover(), because
        // camera_control_init() used to be called once at boot - so a
        // brightness or contrast a user had dialled in silently snapped back
        // to zero on every recovery and every framesize/xclk change, with no
        // log line to explain the picture. Defaults first, then what the user
        // actually chose, on every path that brings the driver up.
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
        sensor->set_whitebal(sensor, 1);
        sensor->set_awb_gain(sensor, 1);
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_hmirror(sensor, 0);
        sensor->set_vflip(sensor, 0);
        camera_control_init();
    }
    return ESP_OK;
}

static size_t camera_dram_fb_bytes(framesize_t fs)
{
    static const struct {
        framesize_t fs;
        uint16_t w;
        uint16_t h;
    } tbl[] = {
        {FRAMESIZE_QQVGA, 160, 120},   {FRAMESIZE_QVGA, 320, 240},
        {FRAMESIZE_VGA, 640, 480},     {FRAMESIZE_SVGA, 800, 600},
        {FRAMESIZE_XGA, 1024, 768},    {FRAMESIZE_HD, 1280, 720},
        {FRAMESIZE_SXGA, 1280, 1024},  {FRAMESIZE_UXGA, 1600, 1200},
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (tbl[i].fs == fs) {
            return (size_t)tbl[i].w * tbl[i].h / 5;
        }
    }
    return 0;
}

static esp_err_t camera_try_init(void)
{
    esp_err_t err = ESP_FAIL;
    for (int round = 0; round < 3; round++) {
        if (round) {
            vTaskDelay(pdMS_TO_TICKS(250 + 250 * round));
        }
        camera_sensor_power_cycle();
        camera_i2c_bus_recovery();
        err = camera_driver_init();
        if (err == ESP_OK) {
            s_driver_up = true;
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_CAMERA_NOT_DETECTED) {
            break;
        }
    }
    s_driver_up = false;
    return err;
}

static void camera_cfg_save(void)
{
    cam_cfg_nvs_t rec = {
        .magic = CAM_CFG_NVS_MAGIC,
        .framesize = (uint16_t)s_framesize,
        .quality = (uint16_t)s_quality,
        .fb_count = (uint16_t)s_fb_count,
        .xclk_mhz = (uint16_t)s_xclk_mhz,
        .grab_mode = (uint8_t)s_grab_mode,
        .fb_location = (uint8_t)s_fb_location,
        .version = CAM_CFG_NVS_VERSION,
        .reserved = 0,
        .crc = 0,
    };
    rec.crc = esp_rom_crc32_le(0, (const uint8_t *)&rec, sizeof(rec) - sizeof(rec.crc));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config save failed: %s", esp_err_to_name(err));
    }
}

static void camera_cfg_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    cam_cfg_nvs_t rec;
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(rec)) {
        return;
    }
    if (rec.magic != CAM_CFG_NVS_MAGIC || rec.version != CAM_CFG_NVS_VERSION) {
        return;
    }
    if (rec.crc != esp_rom_crc32_le(0, (const uint8_t *)&rec, sizeof(rec) - sizeof(rec.crc))) {
        ESP_LOGW(TAG, "stored config failed CRC, using defaults");
        return;
    }
    if (rec.framesize < FRAMESIZE_QQVGA || rec.framesize > FRAMESIZE_UXGA ||
        rec.quality > 63 || rec.fb_count < 1 || rec.fb_count > 3 ||
        rec.xclk_mhz < 6 || rec.xclk_mhz > 27) {
        ESP_LOGW(TAG, "stored config out of range, using defaults");
        return;
    }
    // CP-18: these two are enum tags, not ranges, so a range check cannot
    // catch them and the byte went straight into esp_camera_config_t. The HTTP
    // path has always validated them (camera_apply_config); restore was the
    // one door left open, and it is the door a future schema migration or a
    // hand-edited partition would come through.
    if ((rec.grab_mode != CAMERA_GRAB_LATEST &&
         rec.grab_mode != CAMERA_GRAB_WHEN_EMPTY) ||
        (rec.fb_location != CAMERA_FB_IN_PSRAM &&
         rec.fb_location != CAMERA_FB_IN_DRAM)) {
        ESP_LOGW(TAG, "stored config has an unknown grab mode or fb location, using defaults");
        return;
    }
    // FW-6: the measured ceilings are enforced on the HTTP path but were not
    // on restore, so a stored HD@24 MHz - which Phase 5 measured as producing
    // no frames at all - would come back to life on boot and reopen the FB-OVF
    // class ADR-0009 closed. Reject rather than clamp, because that is what
    // camera_apply_config does with the same inputs: the caller is told the
    // operating point is refused instead of silently running a different one.
    const framesize_t stored_fs = (framesize_t)rec.framesize;
    if (rec.quality < camera_quality_floor(stored_fs)) {
        ESP_LOGW(TAG, "stored quality %d below the measured floor %d for fs=%d, using defaults",
                 rec.quality, camera_quality_floor(stored_fs), (int)stored_fs);
        return;
    }
    if (rec.xclk_mhz > camera_xclk_max_mhz(stored_fs)) {
        ESP_LOGW(TAG, "stored xclk %d above the measured ceiling %d for fs=%d, using defaults",
                 rec.xclk_mhz, camera_xclk_max_mhz(stored_fs), (int)stored_fs);
        return;
    }
    s_framesize = (framesize_t)rec.framesize;
    s_quality = rec.quality;
    s_fb_count = rec.fb_count;
    s_xclk_mhz = rec.xclk_mhz;
    s_grab_mode = (camera_grab_mode_t)rec.grab_mode;
    s_fb_location = (camera_fb_location_t)rec.fb_location;
    ESP_LOGI(TAG, "restored config fs=%d q=%d xclk=%d fb=%d grab=%d loc=%d",
             (int)s_framesize, s_quality, s_xclk_mhz, s_fb_count,
             (int)s_grab_mode, (int)s_fb_location);
}

bool camera_init(void)
{
    if (!s_cam_mutex) {
        s_cam_mutex = xSemaphoreCreateMutex();
        if (!s_cam_mutex) {
            return false;
        }
    }
    camera_cfg_restore();
    return camera_try_init() == ESP_OK;
}

static bool camera_gate_expired(TickType_t deadline)
{
    return (int32_t)(xTaskGetTickCount() - deadline) >= 0;
}

// Called with s_cam_mutex held. Waits for any teardown already running to
// finish, closes the gate so no frame buffer can leave the driver, then waits
// for the ones already out - plus any esp_camera_fb_get() still inside the
// driver - to come back. That wait is the point: esp_camera_deinit() frees the
// frame buffers, so a teardown that did not wait would free memory a client is
// mid-send on. Returns false only if the wait times out, and in that case the
// gate is left open and the caller must not touch the driver.
static bool camera_gate_close(void)
{
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(CAM_TEARDOWN_TIMEOUT_MS);

    while (s_gate_closed) {
        if (camera_gate_expired(deadline)) {
            ESP_LOGE(TAG, "camera teardown: another teardown still running after %d ms",
                     CAM_TEARDOWN_TIMEOUT_MS);
            return false;
        }
        xSemaphoreGive(s_cam_mutex);
        vTaskDelay(pdMS_TO_TICKS(CAM_GATE_POLL_MS));
        xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    }
    s_gate_closed = true;

    while (s_gets_in_flight > 0 || s_fb_held > 0) {
        if (camera_gate_expired(deadline)) {
            ESP_LOGE(TAG, "camera teardown: %d get in flight, %d frame buffer(s) held "
                          "after %d ms - a client socket is not draining",
                     s_gets_in_flight, s_fb_held, CAM_TEARDOWN_TIMEOUT_MS);
            s_gate_closed = false;
            return false;
        }
        xSemaphoreGive(s_cam_mutex);
        vTaskDelay(pdMS_TO_TICKS(CAM_GATE_POLL_MS));
        xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    }
    return true;
}

// Called with s_cam_mutex held, only after a camera_gate_close() that returned
// true, and on every path out of the teardown - including the paths that fail
// after the gate has been taken.
static void camera_gate_open(void)
{
    s_gate_closed = false;
}

esp_err_t camera_recover(void)
{
    if (!s_cam_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    if (!camera_gate_close()) {
        xSemaphoreGive(s_cam_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_driver_up) {
        esp_camera_deinit();
        s_last_fb = NULL;
        s_driver_up = false;
    }
    esp_err_t err = camera_try_init();
    if (err == ESP_OK) {
        s_recovery_count++;
        ESP_LOGW(TAG, "camera recovered (attempt %u)", (unsigned)s_recovery_count);
    } else {
        ESP_LOGE(TAG, "camera recovery failed: %s", esp_err_to_name(err));
    }
    camera_gate_open();
    xSemaphoreGive(s_cam_mutex);
    return err;
}

bool camera_is_up(void)
{
    return s_driver_up;
}

uint32_t camera_recovery_count(void)
{
    return s_recovery_count;
}

camera_fb_t *camera_fb_get(void)
{
    if (!s_cam_mutex) {
        return NULL;
    }

    for (;;) {
        xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
        while (s_gate_closed) {
            // A teardown is running. Wait outside the lock so the task doing
            // it is never queued behind this one - which is the whole point
            // of CP-19, and what the mutex-held-across-a-send design could
            // not do.
            xSemaphoreGive(s_cam_mutex);
            vTaskDelay(pdMS_TO_TICKS(CAM_GATE_POLL_MS));
            xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
        }
        s_gets_in_flight++;
        xSemaphoreGive(s_cam_mutex);

        // Up to four seconds inside the driver with no camera lock held.
        // s_gets_in_flight is what stops a teardown from starting meanwhile.
        camera_fb_t *fb = esp_camera_fb_get();

        xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
        if (fb && !s_gate_closed) {
            s_fb_held++;
            s_last_fb = fb;
            s_gets_in_flight--;
            xSemaphoreGive(s_cam_mutex);
            return fb;
        }
        if (fb) {
            // The teardown began while this frame was being captured. Give it
            // straight back and go round, so the caller sees a wait rather
            // than a failed capture - STREAM_CAPTURE_FAIL_LIMIT is 1, so a
            // NULL here would send the stream handler into recovery.
            esp_camera_fb_return(fb);
        }
        s_gets_in_flight--;
        xSemaphoreGive(s_cam_mutex);

        if (!fb) {
            return NULL; // the camera genuinely produced nothing
        }
    }
}

void camera_fb_return(camera_fb_t *fb)
{
    if (!s_cam_mutex) {
        return;
    }
    xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    if (fb) {
        esp_camera_fb_return(fb);
        if (fb == s_last_fb) {
            s_last_fb = NULL;
        }
        if (s_fb_held > 0) {
            s_fb_held--;
        }
    }
    xSemaphoreGive(s_cam_mutex);
}

framesize_t camera_current_framesize(void) { return s_framesize; }
int camera_current_quality(void) { return s_quality; }
int camera_current_fb_count(void) { return s_fb_count; }
int camera_current_xclk_mhz(void) { return s_xclk_mhz; }
camera_grab_mode_t camera_current_grab_mode(void) { return s_grab_mode; }
camera_fb_location_t camera_current_fb_location(void) { return s_fb_location; }

esp_err_t camera_apply_config(framesize_t fs, int quality, int xclk_mhz,
                              int fb_count, camera_grab_mode_t grab,
                              camera_fb_location_t fb_location)
{
    if (!s_cam_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fb_count < 1 || fb_count > 3 ||
        quality < 0 || quality > 63 ||
        xclk_mhz < 6 || xclk_mhz > 27 ||
        (grab != CAMERA_GRAB_LATEST && grab != CAMERA_GRAB_WHEN_EMPTY) ||
        (fb_location != CAMERA_FB_IN_PSRAM && fb_location != CAMERA_FB_IN_DRAM)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fb_location == CAMERA_FB_IN_DRAM) {
        const size_t per = camera_dram_fb_bytes(fs);
        const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
        if (per &&
            (heap_caps_get_largest_free_block(caps) < per ||
             heap_caps_get_free_size(caps) < per * (size_t)fb_count)) {
            ESP_LOGW(TAG, "dram fb infeasible: need %u x %u bytes internal",
                     (unsigned)per, (unsigned)fb_count);
            return ESP_ERR_NO_MEM;
        }
    }

    const int floor = camera_quality_floor(fs);
    if (quality < floor) {
        ESP_LOGW(TAG, "quality %d below measured safe floor %d for fs=%d (max frame %u, budget %u)",
                 quality, floor, (int)fs, (unsigned)camera_estimate_frame_bytes(fs, floor),
                 (unsigned)CAM_FRAME_BUDGET_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    const int xclk_max = camera_xclk_max_mhz(fs);
    if (xclk_mhz > xclk_max) {
        ESP_LOGW(TAG, "xclk %d MHz above the measured ceiling %d MHz for fs=%d",
                 xclk_mhz, xclk_max, (int)fs);
        return ESP_ERR_INVALID_ARG;
    }

    const framesize_t prev_fs = s_framesize;
    const int prev_quality = s_quality;
    const int prev_xclk = s_xclk_mhz;
    const int prev_fb = s_fb_count;
    const camera_grab_mode_t prev_grab = s_grab_mode;
    const camera_fb_location_t prev_loc = s_fb_location;

    const bool only_quality = s_driver_up && fs == prev_fs && xclk_mhz == prev_xclk &&
                              fb_count == prev_fb && grab == prev_grab &&
                              fb_location == prev_loc && quality != prev_quality;
    if (only_quality) {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor && sensor->set_quality) {
            sensor->set_quality(sensor, quality);
            s_quality = quality;
            ESP_LOGI(TAG, "camera quality set to %d (no reinit)", quality);
            camera_cfg_save();
            return ESP_OK;
        }
    }

    xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    // Wait for consumers to give their frames back before deinit frees them.
    // Without this the wait is implicit - the mutex is held across a frame's
    // whole lifetime, so a slow client stalls every other camera user instead
    // of only this teardown (CP-19).
    if (!camera_gate_close()) {
        xSemaphoreGive(s_cam_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_framesize = fs;
    s_quality = quality;
    s_xclk_mhz = xclk_mhz;
    s_fb_count = fb_count;
    s_grab_mode = grab;
    s_fb_location = fb_location;

    esp_err_t err;
    if (s_driver_up) {
        esp_camera_deinit();
        s_last_fb = NULL;
        s_driver_up = false;
    }
    err = camera_try_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "camera applied fs=%d q=%d xclk=%d fb=%d grab=%d loc=%d "
                      "max_frame_measured=%u budget=%u",
                 (int)fs, quality, xclk_mhz, fb_count, (int)grab, (int)fb_location,
                 (unsigned)camera_estimate_frame_bytes(fs, quality),
                 (unsigned)CAM_FRAME_BUDGET_BYTES);
        camera_cfg_save();
        camera_gate_open();
        xSemaphoreGive(s_cam_mutex);
        return ESP_OK;
    }

    s_framesize = prev_fs;
    s_quality = prev_quality;
    s_xclk_mhz = prev_xclk;
    s_fb_count = prev_fb;
    s_grab_mode = prev_grab;
    s_fb_location = prev_loc;
    esp_err_t rerr = camera_try_init();
    ESP_LOGE(TAG, "camera apply failed (%s), rollback %s",
             esp_err_to_name(err),
             rerr == ESP_OK ? "ok" : esp_err_to_name(rerr));
    camera_gate_open();
    xSemaphoreGive(s_cam_mutex);
    return err;
}
