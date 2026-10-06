set(COMMON_SRC "webrtc.cpp" "main.cpp" "http.cpp")

# Never-flashed features are default-off (t_4f8fe707, see ../CMakeLists.txt).
# Source order matches main's list so an all-on build links identically.
set(TARGET_SRC "wifi.cpp" "media.cpp" "ota.cpp" "rtvi.cpp" "rtvi_callbacks.cpp" "prebuffer_ctl.c" "ack_beep.c" "link_health.cpp")
if("$ENV{PIPECAT_DECIM_COMP}" STREQUAL "1")
	list(APPEND TARGET_SRC "decim_comp.c")
endif()
if("$ENV{PIPECAT_FEC_LBRR}" STREQUAL "1")
	list(APPEND TARGET_SRC "opus_gapfill.c")
endif()
list(APPEND TARGET_SRC "xvf_beam_json.cpp")
if("$ENV{PIPECAT_XVF_AEC_FILTER}" STREQUAL "1")
	list(APPEND TARGET_SRC "xvf_aec_filter.cpp")
endif()

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated"
		REQUIRES peer esp-libopus esp_http_client json)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} ${TARGET_SRC}
		INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated"
		EMBED_FILES "selftest_clip.pcm"
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
