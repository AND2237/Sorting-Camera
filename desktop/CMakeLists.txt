cmake_minimum_required(VERSION 3.21)

project(SortingCamera VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick Network)

qt_standard_project_setup()

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(SortingCamera
    main.cpp
    src/AppMetrics.cpp
    src/AppMetrics.h
    src/DeviceStatus.cpp
    src/DeviceStatus.h
    src/FrameBus.cpp
    src/FrameBus.h
    src/FrameImageProvider.cpp
    src/FrameImageProvider.h
    src/MjpegClient.cpp
    src/MjpegClient.h
)

qt_add_qml_module(SortingCamera
    VERSION 1.0
    URI SortingCamera
    QML_FILES
        qml/Main.qml
)

target_link_libraries(SortingCamera PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Network
)
