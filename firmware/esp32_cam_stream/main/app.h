#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"

#define FW_VERSION       "0.1.0"
#define PROTO_VERSION    1
#define DEVICE_NAME      "sorting-cam-01"

#define CONTROL_HTTP_PORT   80
#define STREAM_HTTP_PORT    81

#define AP_CHANNEL          1
#define AP_MAX_CLIENTS      4
#define AP_IP               "192.168.4.1"

#define STREAM_STALL_TIMEOUT_US   (15LL * 1000 * 1000)

#define FRAME_TCP_PORT   82
#define FRAME_UDP_PORT   8500

#define CAM_DEFAULT_FRAMESIZE     FRAMESIZE_HD
#define CAM_DEFAULT_JPEG_QUALITY  12
#define CAM_DEFAULT_XCLK_HZ       18000000
#define CAM_DEFAULT_FB_COUNT      3

bool camera_init(void);
camera_fb_t *camera_fb_get(void);
void camera_fb_return(camera_fb_t *fb);
framesize_t camera_current_framesize(void);
int camera_current_quality(void);
int camera_current_fb_count(void);
int camera_current_xclk_mhz(void);
bool camera_set_xclk(int mhz);
bool camera_set_framesize(framesize_t fs);
bool camera_set_quality(int quality);

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
void metrics_record_capture(int64_t duration_us, size_t bytes);
void metrics_record_delivery(void);
void metrics_capture_failure(void);
uint32_t metrics_frames_captured(void);
uint32_t metrics_frames_delivered(void);
uint32_t metrics_capture_failures(void);
int64_t metrics_avg_capture_us(void);
size_t metrics_last_frame_bytes(void);
void metrics_start(void);
