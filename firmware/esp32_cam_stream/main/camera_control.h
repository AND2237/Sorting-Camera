#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    CAM_CTRL_BRIGHTNESS = 0,
    CAM_CTRL_CONTRAST,
    CAM_CTRL_SATURATION,
    CAM_CTRL_AE_LEVEL,
    CAM_CTRL_SPECIAL_EFFECT,
    CAM_CTRL_WB_MODE,
    CAM_CTRL_AEC_VALUE,
    CAM_CTRL_AGC_GAIN,
    CAM_CTRL_GAINCEILING,
    CAM_CTRL_AGC,
    CAM_CTRL_AEC,
    CAM_CTRL_AEC2,
    CAM_CTRL_AWB,
    CAM_CTRL_AWB_GAIN,
    CAM_CTRL_HMIRROR,
    CAM_CTRL_VFLIP,
    CAM_CTRL_BPC,
    CAM_CTRL_WPC,
    CAM_CTRL_RAW_GMA,
    CAM_CTRL_LENC,
    CAM_CTRL_DCW,
    CAM_CTRL_COLORBAR,
    CAM_CTRL_SHARPNESS,
    CAM_CTRL_DENOISE,
    CAM_CTRL_COUNT
} cam_ctrl_id_t;

typedef struct {
    const char *name;
    int min;
    int max;
    int def;
    bool supported;
    const char *group;
    const char *note;
} cam_ctrl_def_t;

int camera_control_count(void);
const cam_ctrl_def_t *camera_control_def(int index);
const cam_ctrl_def_t *camera_control_find(const char *name);

esp_err_t camera_control_init(void);
esp_err_t camera_control_apply(cJSON *params, char *err, size_t err_len);
void camera_control_add_status(cJSON *obj);
void camera_control_add_capabilities(cJSON *obj);
