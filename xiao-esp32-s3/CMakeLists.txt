cmake_minimum_required(VERSION 3.19)

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()
endif()

if(NOT DEFINED ENV{PIPECAT_SMALLWEBRTC_URL})
  message(FATAL_ERROR "Env variable PIPECAT_SMALLWEBRTC_URL must be set")
endif()

if(DEFINED ENV{LOG_DATACHANNEL_MESSAGES})
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
endif()

if("$ENV{PIPECAT_BENCH_SEND_TONE}" STREQUAL "1")
  add_compile_definitions(PIPECAT_BENCH_SEND_TONE=1)
endif()

set(WIFI_SSID_VALUE "$ENV{WIFI_SSID}")
set(WIFI_PASSWORD_VALUE "$ENV{WIFI_PASSWORD}")
set(PIPECAT_SMALLWEBRTC_URL_VALUE "$ENV{PIPECAT_SMALLWEBRTC_URL}")
set(PIPECAT_SATELLITE_ID_VALUE "$ENV{PIPECAT_SATELLITE_ID}")
set(PIPECAT_MDNS_HOSTNAME_VALUE "$ENV{PIPECAT_MDNS_HOSTNAME}")
set(PIPECAT_MDNS_INSTANCE_VALUE "$ENV{PIPECAT_MDNS_INSTANCE}")
set(PIPECAT_AEC_FAR_EXTGAIN_DB_VALUE "$ENV{PIPECAT_AEC_FAR_EXTGAIN_DB}")
set(PIPECAT_AGC_DESIRED_LEVEL_VALUE "$ENV{PIPECAT_AGC_DESIRED_LEVEL}")

if("${PIPECAT_SATELLITE_ID_VALUE}" STREQUAL "")
  set(PIPECAT_SATELLITE_ID_VALUE "bench")
endif()

if("${PIPECAT_MDNS_HOSTNAME_VALUE}" STREQUAL "")
  set(PIPECAT_MDNS_HOSTNAME_VALUE "${PIPECAT_SATELLITE_ID_VALUE}-xvf3800")
endif()

if("${PIPECAT_MDNS_INSTANCE_VALUE}" STREQUAL "")
  set(PIPECAT_MDNS_INSTANCE_VALUE "${PIPECAT_SATELLITE_ID_VALUE} XVF3800 Voice Satellite")
endif()

if("${PIPECAT_AEC_FAR_EXTGAIN_DB_VALUE}" STREQUAL "")
  set(PIPECAT_AEC_FAR_EXTGAIN_DB_VALUE "0.0f")
endif()

# XVF3800 AGC target output level. Default 0.0045f = the XMOS/Seeed stock value
# (quiet far-field). Raise it (e.g. 0.015f) to make AGC boost far-field speech
# louder toward the wake threshold. Env: PIPECAT_AGC_DESIRED_LEVEL.
if("${PIPECAT_AGC_DESIRED_LEVEL_VALUE}" STREQUAL "")
  set(PIPECAT_AGC_DESIRED_LEVEL_VALUE "0.0045f")
endif()

foreach(secret_var
    WIFI_SSID_VALUE
    WIFI_PASSWORD_VALUE
    PIPECAT_SMALLWEBRTC_URL_VALUE
    PIPECAT_SATELLITE_ID_VALUE
    PIPECAT_MDNS_HOSTNAME_VALUE
    PIPECAT_MDNS_INSTANCE_VALUE)
  string(REPLACE "\\" "\\\\" ${secret_var} "${${secret_var}}")
  string(REPLACE "\"" "\\\"" ${secret_var} "${${secret_var}}")
endforeach()

set(PIPECAT_GENERATED_CONFIG_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PIPECAT_GENERATED_CONFIG_DIR}")
configure_file(
  "${CMAKE_SOURCE_DIR}/src/pipecat_build_config.h.in"
  "${PIPECAT_GENERATED_CONFIG_DIR}/pipecat_build_config.h"
  @ONLY
)

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "../esp32-s3-box-3/components/srtp" "components/peer" "../esp32-s3-box-3/components/esp-libopus")

if(IDF_TARGET STREQUAL linux)
  add_compile_definitions(LINUX_BUILD=1)
  list(APPEND EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/protocols/linux_stubs/esp_stubs
    "components/esp-protocols/common_components/linux_compat/esp_timer"
    "components/esp-protocols/common_components/linux_compat/freertos"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(src)
