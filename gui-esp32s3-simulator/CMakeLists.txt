cmake_minimum_required(VERSION 3.21)
project(gui_esp32s3_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)

add_executable(gui_esp32s3_simulator
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/SerialConsoleWidget.h
    src/SerialConsoleWidget.cpp
    src/CpuStatusWidget.h
    src/CpuStatusWidget.cpp
    src/ControlPanelWidget.h
    src/ControlPanelWidget.cpp
    src/DebugWidget.h
    src/DebugWidget.cpp
    backend/QemuController.h
    backend/QemuController.cpp
)

target_include_directories(gui_esp32s3_simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(gui_esp32s3_simulator PRIVATE Qt6::Widgets Qt6::Core Qt6::Network)

if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_simulator PRIVATE util)
endif()
