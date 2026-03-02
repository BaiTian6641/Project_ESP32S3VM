cmake_minimum_required(VERSION 3.21)
project(gui_esp32s3_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network Test)

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
    src/PeripheralsWidget.h
    src/PeripheralsWidget.cpp
    src/DevicePanelBase.h
    src/DevicePanelBase.cpp
    src/DevicePanelFactory.h
    src/DevicePanelFactory.cpp
    src/PinConnectionWidget.h
    src/PinConnectionWidget.cpp
    src/DisplayPanel.h
    src/DisplayPanel.cpp
    src/SensorPanel.h
    src/SensorPanel.cpp
    src/SpiFlashPanel.h
    src/SpiFlashPanel.cpp
    src/UartDevicePanel.h
    src/UartDevicePanel.cpp
    src/GenericDevicePanel.h
    src/GenericDevicePanel.cpp
    backend/QemuController.h
    backend/QemuController.cpp
    backend/PeripheralManager.h
    backend/PeripheralManager.cpp
)

target_include_directories(gui_esp32s3_simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(gui_esp32s3_simulator PRIVATE Qt6::Widgets Qt6::Core Qt6::Network)

if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_simulator PRIVATE util)
endif()

enable_testing()

add_executable(gui_esp32s3_bridge_contract_test
    tests/BridgeContractTest.cpp
    backend/QemuController.h
    backend/QemuController.cpp
)

target_include_directories(gui_esp32s3_bridge_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(gui_esp32s3_bridge_contract_test PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_bridge_contract_test PRIVATE util)
endif()

add_test(NAME bridge_contract_test COMMAND gui_esp32s3_bridge_contract_test)
