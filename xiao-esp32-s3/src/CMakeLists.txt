set(COMMON_SRC "webrtc.cpp" "main.cpp" "http.cpp")

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated"
		REQUIRES peer esp-libopus esp_http_client json)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} "wifi.cpp" "media.cpp" "ota.cpp" "rtvi.cpp" "rtvi_callbacks.cpp"
		INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated"
		REQUIRES app_update driver esp_timer esp_wifi esp_http_server mdns nvs_flash peer esp_psram esp-libopus esp_http_client json)
endif()

idf_component_get_property(lib peer COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=restrict)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-truncation)

idf_component_get_property(lib srtp COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(lib esp-libopus COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=maybe-uninitialized)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-overread)
