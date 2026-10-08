cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED ENV{ESP32S3_ARDUINO_SOURCE})
    message(FATAL_ERROR "Set ESP32S3_ARDUINO_SOURCE to the existing pinned Arduino3.3.12 component")
endif()
# The existing selective arduino_boot profile requires only SDK components.
# No registry/dependency resolution or source download is permitted here.
set(ENV{IDF_COMPONENT_MANAGER} 0)
set(EXTRA_COMPONENT_DIRS "$ENV{ESP32S3_ARDUINO_SOURCE}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(uart_native_arduino)
