cmake_minimum_required(VERSION 3.16)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.h")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.example.h"
        "${CMAKE_CURRENT_LIST_DIR}/main/config_secrets.h"
        COPYONLY
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_cam_stream)
