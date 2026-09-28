cmake_minimum_required(VERSION 3.21)

project(SortingCamera VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Test)

qt_standard_project_setup()

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_library(scamcore STATIC
    src/AuthClient.cpp
    src/AuthClient.h
    src/CredentialStore.cpp
    src/CredentialStore.h
    src/DiscoveryService.cpp
    src/DiscoveryService.h
    src/DeviceStatus.cpp
    src/DeviceStatus.h
    src/FrameBus.cpp
    src/FrameBus.h
    src/MjpegClient.cpp
    src/MjpegClient.h
    src/SessionState.cpp
    src/SessionState.h
    src/Recorder.cpp
    src/Recorder.h
    src/SnapshotWriter.cpp
    src/SnapshotWriter.h
    src/StreamStats.cpp
    src/StreamStats.h
    src/CameraDevice.cpp
    src/CameraDevice.h
    src/Diagnostics.cpp
    src/Diagnostics.h
    src/NotificationCenter.cpp
    src/NotificationCenter.h
    src/ProfileEngine.cpp
    src/ProfileEngine.h
    src/DeviceRegistry.cpp
    src/DeviceRegistry.h
    src/UserPreferences.cpp
    src/UserPreferences.h
    src/AppMetrics.cpp
    src/AppMetrics.h
)

target_include_directories(scamcore PUBLIC src)

target_link_libraries(scamcore PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

if(WIN32)
    target_link_libraries(scamcore PUBLIC crypt32)
endif()

qt_add_executable(SortingCamera
    main.cpp
    src/FrameImageProvider.cpp
    src/FrameImageProvider.h
)

target_link_libraries(SortingCamera PRIVATE
    scamcore
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

# Theme is a singleton, not an instantiable component: the design tokens have
# to be one object per application, shared by everything that draws. The
# property has to be set before qt_add_qml_module reads the file list.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(SortingCamera
        VERSION 1.0
        URI SortingCamera
        QML_FILES
            qml/Theme.qml
            qml/Main.qml
    )

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
