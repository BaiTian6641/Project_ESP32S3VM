idf_component_register(SRCS "simd_reference.c" "pie_reference.S" INCLUDE_DIRS "." REQUIRES esp_system esp_app_format)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/simd_reference.c" REFERENCE_SOURCE_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/simd_reference.c")
target_compile_definitions(${COMPONENT_LIB} PRIVATE REFERENCE_SOURCE_SHA256="${REFERENCE_SOURCE_SHA256}")
