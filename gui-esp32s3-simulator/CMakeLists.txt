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
    src/ThemeManager.h
    src/ThemeManager.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/BoardWorkspace.cpp
    src/BoardWorkspace.h
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
    src/SchemaDevicePanel.h
    src/SchemaDevicePanel.cpp
    backend/QemuController.h
    backend/QemuController.cpp
    backend/RuntimeContract.h
    backend/RuntimeContract.cpp
    backend/ProjectDocument.h
    backend/ProjectDocument.cpp
    backend/PeripheralTransport.h
    backend/PeripheralTransport.cpp
    backend/QemuLaunchOptions.cpp
    backend/PeripheralManager.h
    backend/PeripheralManager.cpp
)

if(APPLE)
    set_target_properties(gui_esp32s3_simulator PROPERTIES
        MACOSX_BUNDLE TRUE
    )
endif()

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
    backend/RuntimeContract.cpp
    backend/QemuLaunchOptions.cpp
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

add_executable(gui_esp32s3_launch_options_test tests/LaunchOptionsTest.cpp backend/QemuLaunchOptions.cpp)
target_include_directories(gui_esp32s3_launch_options_test PRIVATE backend)
target_link_libraries(gui_esp32s3_launch_options_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME launch_options_test COMMAND gui_esp32s3_launch_options_test)

add_executable(gui_esp32s3_boot_smoke_test tests/BootSmokeTest.cpp backend/QemuController.cpp backend/QemuLaunchOptions.cpp backend/RuntimeContract.cpp)
target_include_directories(gui_esp32s3_boot_smoke_test PRIVATE backend)
target_link_libraries(gui_esp32s3_boot_smoke_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_boot_smoke_test PRIVATE util)
endif()
add_test(NAME boot_smoke_test COMMAND gui_esp32s3_boot_smoke_test)
set_tests_properties(boot_smoke_test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP.*ESP32S3_BOOT_FIRMWARE")

add_executable(gui_esp32s3_circuit_workspace_test tests/CircuitWorkspaceTest.cpp src/BoardWorkspace.cpp src/ThemeManager.cpp backend/PeripheralManager.cpp backend/ProjectDocument.cpp backend/RuntimeContract.cpp)
target_include_directories(gui_esp32s3_circuit_workspace_test PRIVATE backend src)
target_compile_definitions(gui_esp32s3_circuit_workspace_test PRIVATE SIMULATOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gui_esp32s3_circuit_workspace_test PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME circuit_workspace_test COMMAND gui_esp32s3_circuit_workspace_test)
set_tests_properties(circuit_workspace_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(gui_esp32s3_native_i2c_test tests/NativeI2cTest.cpp backend/QemuController.cpp backend/QemuLaunchOptions.cpp backend/RuntimeContract.cpp backend/PeripheralManager.cpp)
target_include_directories(gui_esp32s3_native_i2c_test PRIVATE backend)
target_compile_definitions(gui_esp32s3_native_i2c_test PRIVATE SIMULATOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gui_esp32s3_native_i2c_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_native_i2c_test PRIVATE util)
endif()
add_test(NAME native_i2c_test COMMAND gui_esp32s3_native_i2c_test)
set_tests_properties(native_i2c_test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP.*ESP32S3_I2C_FIRMWARE")

add_executable(gui_esp32s3_theme_test tests/ThemeManagerTest.cpp src/ThemeManager.cpp
    src/BoardWorkspace.cpp backend/PeripheralManager.cpp backend/ProjectDocument.cpp backend/RuntimeContract.cpp src/DevicePanelBase.cpp
    src/DisplayPanel.cpp src/PinConnectionWidget.cpp)
target_include_directories(gui_esp32s3_theme_test PRIVATE backend src)
target_compile_definitions(gui_esp32s3_theme_test PRIVATE SIMULATOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gui_esp32s3_theme_test PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME theme_test COMMAND gui_esp32s3_theme_test)
set_tests_properties(theme_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(gui_esp32s3_runtime_contract_test tests/RuntimeContractTest.cpp
    backend/QemuController.cpp backend/QemuLaunchOptions.cpp backend/RuntimeContract.cpp)
target_include_directories(gui_esp32s3_runtime_contract_test PRIVATE backend)
target_link_libraries(gui_esp32s3_runtime_contract_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_runtime_contract_test PRIVATE util)
endif()
add_test(NAME runtime_contract_test COMMAND gui_esp32s3_runtime_contract_test)

add_executable(gui_esp32s3_qt_electrical_bridge_test tests/QtElectricalBridgeTest.cpp
    backend/QemuController.cpp backend/QemuLaunchOptions.cpp backend/RuntimeContract.cpp)
target_include_directories(gui_esp32s3_qt_electrical_bridge_test PRIVATE backend)
target_link_libraries(gui_esp32s3_qt_electrical_bridge_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(UNIX AND NOT APPLE)
    target_link_libraries(gui_esp32s3_qt_electrical_bridge_test PRIVATE util)
endif()
add_test(NAME qt_electrical_bridge_test COMMAND gui_esp32s3_qt_electrical_bridge_test)

add_executable(gui_esp32s3_project_document_test tests/ProjectDocumentTest.cpp backend/ProjectDocument.cpp backend/PeripheralTransport.cpp)
target_include_directories(gui_esp32s3_project_document_test PRIVATE backend)
target_link_libraries(gui_esp32s3_project_document_test PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME project_document_test COMMAND gui_esp32s3_project_document_test)
set_tests_properties(project_document_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(gui_esp32s3_peripheral_transport_test tests/PeripheralTransportTest.cpp backend/PeripheralTransport.cpp)
target_include_directories(gui_esp32s3_peripheral_transport_test PRIVATE backend)
target_link_libraries(gui_esp32s3_peripheral_transport_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME peripheral_transport_test COMMAND gui_esp32s3_peripheral_transport_test)
