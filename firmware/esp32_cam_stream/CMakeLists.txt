cmake_minimum_required(VERSION 3.16)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.h")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.example.h"
        "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.h"
        COPYONLY
    )
endif()

# Image/project version: the root VERSION file, the single source the desktop
# build and scripts\package-release.ps1 already read (G-10). Set explicitly so
# ESP-IDF does not take the version from `git describe` instead.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" SCAM_VERSION LIMIT_COUNT 1)
string(STRIP "${SCAM_VERSION}" SCAM_VERSION)
if(NOT SCAM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must hold MAJOR.MINOR.PATCH, got '${SCAM_VERSION}'")
endif()
set(PROJECT_VER "${SCAM_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cam_stream)
