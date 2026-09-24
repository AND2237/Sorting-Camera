#include "app.h"

#include <string.h>

#include "camera_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "camera";

static SemaphoreHandle_t s_cam_mutex;
static camera_fb_t *s_last_fb;
static int s_fb_count = CAM_DEFAULT_FB_COUNT;
static int s_quality = CAM_DEFAULT_JPEG_QUALITY;
static int s_xclk_mhz = CAM_DEFAULT_XCLK_HZ / 1000000;
static framesize_t s_framesize = CAM_DEFAULT_FRAMESIZE;

bool camera_init(void)
{
    s_cam_mutex = xSemaphoreCreateMutex();
    if (!s_cam_mutex) {
        return false;
    }

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
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "camera init in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
        sensor->set_whitebal(sensor, 1);
        sensor->set_awb_gain(sensor, 1);
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_hmirror(sensor, 0);
        sensor->set_vflip(sensor, 0);
    }
    return true;
}

camera_fb_t *camera_fb_get(void)
{
    if (!s_cam_mutex) {
        return NULL;
    }
    xSemaphoreTake(s_cam_mutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        s_last_fb = fb;
    } else {
        xSemaphoreGive(s_cam_mutex);
    }
    return fb;
}

void camera_fb_return(camera_fb_t *fb)
{
    if (!s_cam_mutex) {
        return;
    }
    if (fb) {
        esp_camera_fb_return(fb);
        if (fb == s_last_fb) {
            s_last_fb = NULL;
        }
    }
    xSemaphoreGive(s_cam_mutex);
}

framesize_t camera_current_framesize(void) { return s_framesize; }
int camera_current_quality(void) { return s_quality; }
int camera_current_fb_count(void) { return s_fb_count; }
int camera_current_xclk_mhz(void) { return s_xclk_mhz; }

bool camera_set_xclk(int mhz)
{
    if (mhz < 6 || mhz > 27) {
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_xclk) {
        return false;
    }
    if (sensor->set_xclk(sensor, LEDC_TIMER_0, mhz) != 0) {
        return false;
    }
    s_xclk_mhz = mhz;
    return true;
}

bool camera_set_framesize(framesize_t fs)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_framesize) {
        return false;
    }
    if (sensor->set_framesize(sensor, fs) != 0) {
        return false;
    }
    s_framesize = fs;
    return true;
}

bool camera_set_quality(int quality)
{
    if (quality < 0 || quality > 63) {
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_quality) {
        return false;
    }
    if (sensor->set_quality(sensor, quality) != 0) {
        return false;
    }
    s_quality = quality;
    return true;
}
