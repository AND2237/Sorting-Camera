#include "camera_control.h"

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "camctl";

static const char *NVS_NS = "camctrl";
static const char *NVS_KEY = "v1";

#define CAM_CTRL_NVS_MAGIC   0x43414D32u
#define CAM_CTRL_NVS_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    int16_t values[CAM_CTRL_COUNT];
} cam_ctrl_nvs_t;

static const cam_ctrl_def_t s_defs[CAM_CTRL_COUNT] = {
    [CAM_CTRL_BRIGHTNESS]     = {"brightness", -2, 2, 0, true, "image", NULL},
    [CAM_CTRL_CONTRAST]       = {"contrast", -2, 2, 0, true, "image", NULL},
    [CAM_CTRL_SATURATION]     = {"saturation", -2, 2, 0, true, "image", NULL},
    [CAM_CTRL_AE_LEVEL]       = {"ae_level", -2, 2, 0, true, "image", NULL},
    [CAM_CTRL_SPECIAL_EFFECT] = {"special_effect", 0, 6, 0, true, "image", NULL},
    [CAM_CTRL_WB_MODE]        = {"wb_mode", 0, 4, 0, true, "white_balance", NULL},
    [CAM_CTRL_AEC_VALUE]      = {"aec_value", 0, 1200, 300, true, "exposure", "manual exposure time, ignored while aec is on"},
    [CAM_CTRL_AGC_GAIN]       = {"agc_gain", 0, 30, 0, true, "gain", "manual analog gain, ignored while agc is on"},
    [CAM_CTRL_GAINCEILING]    = {"gainceiling", 0, 6, 0, true, "gain", "2x 4x 8x 16x 32x 64x 128x"},
    [CAM_CTRL_AGC]            = {"agc", 0, 1, 1, true, "gain", "automatic gain control"},
    [CAM_CTRL_AEC]            = {"aec", 0, 1, 1, true, "exposure", "automatic exposure control"},
    [CAM_CTRL_AEC2]           = {"aec2", 0, 1, 1, true, "exposure", NULL},
    [CAM_CTRL_AWB]            = {"awb", 0, 1, 1, true, "white_balance", "automatic white balance"},
    [CAM_CTRL_AWB_GAIN]       = {"awb_gain", 0, 1, 1, true, "white_balance", "automatic white balance gain"},
    [CAM_CTRL_HMIRROR]        = {"hmirror", 0, 1, 0, true, "orientation", NULL},
    [CAM_CTRL_VFLIP]          = {"vflip", 0, 1, 0, true, "orientation", NULL},
    [CAM_CTRL_BPC]            = {"bpc", 0, 1, 1, true, "processing", "black pixel correction"},
    [CAM_CTRL_WPC]            = {"wpc", 0, 1, 1, true, "processing", "white pixel correction"},
    [CAM_CTRL_RAW_GMA]        = {"raw_gma", 0, 1, 1, true, "processing", "raw gamma"},
    [CAM_CTRL_LENC]           = {"lenc", 0, 1, 1, true, "processing", "lens correction"},
    [CAM_CTRL_DCW]            = {"dcw", 0, 1, 1, true, "processing", "DCW"},
    [CAM_CTRL_COLORBAR]       = {"colorbar", 0, 1, 0, true, "diagnostic", "test pattern, not a product feature"},
    [CAM_CTRL_SHARPNESS]      = {"sharpness", -2, 2, 0, false, "image", "OV2640 driver stub returns -1, not implemented in hardware"},
    [CAM_CTRL_DENOISE]        = {"denoise", 0, 0, 0, false, "processing", "OV2640 driver stub returns -1, not implemented in hardware"},
};

static int read_value(sensor_t *s, int id)
{
    switch (id) {
    case CAM_CTRL_BRIGHTNESS: return s->status.brightness;
    case CAM_CTRL_CONTRAST: return s->status.contrast;
    case CAM_CTRL_SATURATION: return s->status.saturation;
    case CAM_CTRL_AE_LEVEL: return s->status.ae_level;
    case CAM_CTRL_SPECIAL_EFFECT: return s->status.special_effect;
    case CAM_CTRL_WB_MODE: return s->status.wb_mode;
    case CAM_CTRL_AEC_VALUE: return s->status.aec_value;
    case CAM_CTRL_AGC_GAIN: return s->status.agc_gain;
    case CAM_CTRL_GAINCEILING: return s->status.gainceiling;
    case CAM_CTRL_AGC: return s->status.agc;
    case CAM_CTRL_AEC: return s->status.aec;
    case CAM_CTRL_AEC2: return s->status.aec2;
    case CAM_CTRL_AWB: return s->status.awb;
    case CAM_CTRL_AWB_GAIN: return s->status.awb_gain;
    case CAM_CTRL_HMIRROR: return s->status.hmirror;
    case CAM_CTRL_VFLIP: return s->status.vflip;
    case CAM_CTRL_BPC: return s->status.bpc;
    case CAM_CTRL_WPC: return s->status.wpc;
    case CAM_CTRL_RAW_GMA: return s->status.raw_gma;
    case CAM_CTRL_LENC: return s->status.lenc;
    case CAM_CTRL_DCW: return s->status.dcw;
    case CAM_CTRL_COLORBAR: return s->status.colorbar;
    default: return 0;
    }
}

static int write_value(sensor_t *s, int id, int value)
{
    switch (id) {
    case CAM_CTRL_BRIGHTNESS: return s->set_brightness(s, value);
    case CAM_CTRL_CONTRAST: return s->set_contrast(s, value);
    case CAM_CTRL_SATURATION: return s->set_saturation(s, value);
    case CAM_CTRL_AE_LEVEL: return s->set_ae_level(s, value);
    case CAM_CTRL_SPECIAL_EFFECT: return s->set_special_effect(s, value);
    case CAM_CTRL_WB_MODE: return s->set_wb_mode(s, value);
    case CAM_CTRL_AEC_VALUE: return s->set_aec_value(s, value);
    case CAM_CTRL_AGC_GAIN: return s->set_agc_gain(s, value);
    case CAM_CTRL_GAINCEILING: return s->set_gainceiling(s, (gainceiling_t)value);
    case CAM_CTRL_AGC: return s->set_gain_ctrl(s, value);
    case CAM_CTRL_AEC: return s->set_exposure_ctrl(s, value);
    case CAM_CTRL_AEC2: return s->set_aec2(s, value);
    case CAM_CTRL_AWB: return s->set_whitebal(s, value);
    case CAM_CTRL_AWB_GAIN: return s->set_awb_gain(s, value);
    case CAM_CTRL_HMIRROR: return s->set_hmirror(s, value);
    case CAM_CTRL_VFLIP: return s->set_vflip(s, value);
    case CAM_CTRL_BPC: return s->set_bpc(s, value);
    case CAM_CTRL_WPC: return s->set_wpc(s, value);
    case CAM_CTRL_RAW_GMA: return s->set_raw_gma(s, value);
    case CAM_CTRL_LENC: return s->set_lenc(s, value);
    case CAM_CTRL_DCW: return s->set_dcw(s, value);
    case CAM_CTRL_COLORBAR: return s->set_colorbar(s, value);
    default: return -1;
    }
}

int camera_control_count(void)
{
    return CAM_CTRL_COUNT;
}

const cam_ctrl_def_t *camera_control_def(int index)
{
    if (index < 0 || index >= CAM_CTRL_COUNT) {
        return NULL;
    }
    return &s_defs[index];
}

const cam_ctrl_def_t *camera_control_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < CAM_CTRL_COUNT; i++) {
        if (strcmp(s_defs[i].name, name) == 0) {
            return &s_defs[i];
        }
    }
    return NULL;
}

static void load_nvs(int16_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    cam_ctrl_nvs_t rec;
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(rec)) {
        return;
    }
    if (rec.magic != CAM_CTRL_NVS_MAGIC || rec.version != CAM_CTRL_NVS_VERSION) {
        return;
    }
    memcpy(out, rec.values, sizeof(rec.values));
}

static void save_nvs(const int16_t *values)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    cam_ctrl_nvs_t rec = {
        .magic = CAM_CTRL_NVS_MAGIC,
        .version = CAM_CTRL_NVS_VERSION,
    };
    memcpy(rec.values, values, sizeof(rec.values));
    if (nvs_set_blob(h, NVS_KEY, &rec, sizeof(rec)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

esp_err_t camera_control_init(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return ESP_FAIL;
    }

    int16_t stored[CAM_CTRL_COUNT];
    memset(stored, 0, sizeof(stored));
    load_nvs(stored);

    int applied = 0;
    for (int i = 0; i < CAM_CTRL_COUNT; i++) {
        if (!s_defs[i].supported) {
            continue;
        }
        int value = stored[i];
        if (value < s_defs[i].min || value > s_defs[i].max) {
            value = s_defs[i].def;
        }
        if (write_value(s, i, value) == 0) {
            stored[i] = (int16_t)value;
            applied++;
        }
    }
    if (applied > 0) {
        save_nvs(stored);
    }
    ESP_LOGI(TAG, "sensor controls restored from NVS: %d applied", applied);
    return ESP_OK;
}

esp_err_t camera_control_apply(cJSON *params, char *err, size_t err_len)
{
    if (!params || !cJSON_IsObject(params)) {
        snprintf(err, err_len, "body must be a JSON object");
        return ESP_FAIL;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        snprintf(err, err_len, "camera is not initialised");
        return ESP_FAIL;
    }

    struct {
        int id;
        int value;
        int previous;
    } plan[CAM_CTRL_COUNT];
    int planned = 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, params)
    {
        if (!item->string) {
            continue;
        }
        const cam_ctrl_def_t *def = camera_control_find(item->string);
        if (!def) {
            snprintf(err, err_len, "unknown control '%s'", item->string);
            return ESP_FAIL;
        }
        if (!def->supported) {
            snprintf(err, err_len, "control '%s' is not supported: %s", def->name,
                     def->note ? def->note : "driver reports no support");
            return ESP_FAIL;
        }
        int value;
        if (cJSON_IsBool(item)) {
            value = cJSON_IsTrue(item) ? 1 : 0;
        } else if (cJSON_IsNumber(item)) {
            value = (int)item->valuedouble;
        } else {
            snprintf(err, err_len, "control '%s' expects a number or boolean", def->name);
            return ESP_FAIL;
        }
        if (value < def->min || value > def->max) {
            snprintf(err, err_len, "control '%s' out of range: %d not in [%d, %d]", def->name,
                     value, def->min, def->max);
            return ESP_FAIL;
        }
        plan[planned].id = (int)(def - s_defs);
        plan[planned].value = value;
        plan[planned].previous = read_value(s, plan[planned].id);
        planned++;
    }

    if (planned == 0) {
        snprintf(err, err_len, "no controls in request");
        return ESP_FAIL;
    }

    int done = 0;
    for (; done < planned; done++) {
        if (write_value(s, plan[done].id, plan[done].value) != 0) {
            snprintf(err, err_len, "control '%s' rejected by the sensor driver",
                     s_defs[plan[done].id].name);
            for (int i = done - 1; i >= 0; i--) {
                write_value(s, plan[i].id, plan[i].previous);
            }
            return ESP_FAIL;
        }
    }

    int16_t values[CAM_CTRL_COUNT];
    for (int i = 0; i < CAM_CTRL_COUNT; i++) {
        values[i] = (int16_t)read_value(s, i);
    }
    save_nvs(values);
    ESP_LOGI(TAG, "applied %d sensor control(s)", planned);
    return ESP_OK;
}

void camera_control_add_status(cJSON *obj)
{
    sensor_t *s = esp_camera_sensor_get();
    for (int i = 0; i < CAM_CTRL_COUNT; i++) {
        if (!s_defs[i].supported) {
            continue;
        }
        cJSON_AddNumberToObject(obj, s_defs[i].name, s ? read_value(s, i) : s_defs[i].def);
    }
}

void camera_control_add_capabilities(cJSON *obj)
{
    sensor_t *s = esp_camera_sensor_get();
    cJSON *controls = cJSON_AddObjectToObject(obj, "controls");
    for (int i = 0; i < CAM_CTRL_COUNT; i++) {
        cJSON *entry = cJSON_AddObjectToObject(controls, s_defs[i].name);
        cJSON_AddBoolToObject(entry, "supported", s_defs[i].supported);
        cJSON_AddStringToObject(entry, "group", s_defs[i].group);
        cJSON_AddNumberToObject(entry, "min", s_defs[i].min);
        cJSON_AddNumberToObject(entry, "max", s_defs[i].max);
        cJSON_AddNumberToObject(entry, "default", s_defs[i].def);
        if (s_defs[i].note) {
            cJSON_AddStringToObject(entry, "note", s_defs[i].note);
        }
    }

    cJSON *features = cJSON_AddObjectToObject(obj, "features");
    cJSON_AddBoolToObject(features, "autofocus", false);
    cJSON_AddBoolToObject(features, "raw_register_access", false);
    cJSON_AddBoolToObject(features, "sensor_controls_live", true);
    cJSON_AddBoolToObject(features, "quality_live", true);

    (void)s;
}
