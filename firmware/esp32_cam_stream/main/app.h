#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"

#define FW_VERSION       "0.2.0"
#define PROTO_VERSION    1
#define DEVICE_NAME      "sorting-cam-01"

#define CONTROL_HTTP_PORT   80
#define STREAM_HTTP_PORT    81
#define DISCOVERY_UDP_PORT  48888

#define AP_CHANNEL          1
#define AP_MAX_CLIENTS      4
#define AP_IP               "192.168.4.1"

#define STREAM_STALL_TIMEOUT_US   (15LL * 1000 * 1000)
#define CAMERA_DEAD_TIMEOUT_US    (30LL * 1000 * 1000)
#define STREAM_CAPTURE_FAIL_LIMIT 1

#define FRAME_TCP_PORT   82
#define FRAME_UDP_PORT   8500

#define CAM_DEFAULT_FRAMESIZE     FRAMESIZE_HD
#define CAM_DEFAULT_JPEG_QUALITY  12
#define CAM_DEFAULT_XCLK_HZ       18000000
#define CAM_DEFAULT_FB_COUNT      3
#define CAM_DEFAULT_GRAB_MODE     CAMERA_GRAB_LATEST
#define CAM_DEFAULT_FB_LOCATION   CAMERA_FB_IN_PSRAM

bool camera_init(void);
camera_fb_t *camera_fb_get(void);
void camera_fb_return(camera_fb_t *fb);
const char *device_id_hex(void);
const char *device_ip(void);
framesize_t camera_current_framesize(void);
int camera_current_quality(void);
int camera_current_fb_count(void);
int camera_current_xclk_mhz(void);
camera_grab_mode_t camera_current_grab_mode(void);
camera_fb_location_t camera_current_fb_location(void);
esp_err_t camera_apply_config(framesize_t fs, int quality, int xclk_mhz,
                              int fb_count, camera_grab_mode_t grab,
                              camera_fb_location_t fb_location);
esp_err_t camera_recover(void);
bool camera_is_up(void);
uint32_t camera_recovery_count(void);
size_t camera_frame_budget(void);
uint32_t camera_estimate_frame_bytes(framesize_t fs, int quality);
int camera_quality_floor(framesize_t fs);
int camera_xclk_max_mhz(framesize_t fs);

bool wifi_start(const char *ssid, const char *password);
int wifi_get_rssi(void);

esp_err_t start_control_server(void);
esp_err_t start_stream_server(void);
int stream_client_count(void);

bool frame_transport_start(void);
int frame_transport_client_count(void);
int frame_transport_tcp_clients(void);
int frame_transport_udp_peer(void);
uint32_t frame_transport_udp_tx_dgrams(void);
uint32_t frame_transport_udp_tx_drops(void);

void metrics_mark_stream_active(void);
int64_t metrics_us_since_last_delivery(void);
int64_t metrics_us_since_last_capture(void);
void metrics_record_capture(int64_t duration_us, size_t bytes);
void metrics_record_delivery(void);
void metrics_capture_failure(void);
uint32_t metrics_frames_captured(void);
uint32_t metrics_frames_delivered(void);
uint32_t metrics_capture_failures(void);
int64_t metrics_avg_capture_us(void);
size_t metrics_last_frame_bytes(void);
void metrics_start(void);
