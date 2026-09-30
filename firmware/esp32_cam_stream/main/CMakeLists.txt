idf_component_register(
    SRCS
        "app_main.c"
        "auth.c"
        "camera.c"
        "camera_control.c"
        "discovery.c"
        "wifi.c"
        "http_servers.c"
        "frame_transport.c"
        "metrics.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp32-camera
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        esp_psram
        nvs_flash
        esp_timer
        json
        esp_app_format
        mbedtls
)

# CP-24 / G-10: the firmware version comes from the repository's single VERSION
# file, the same one the desktop project and the release script read, so the
# fw_version on the wire cannot drift from the app's own version. app.h keeps a
# "dev" fallback for anyone bypassing this component build.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../../VERSION" SCAM_VERSION LIMIT_COUNT 1)
string(STRIP "${SCAM_VERSION}" SCAM_VERSION)
if(NOT SCAM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must hold MAJOR.MINOR.PATCH, got '${SCAM_VERSION}'")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${SCAM_VERSION}")
