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

if("$ENV{PIPECAT_DUAL_STREAM}" STREQUAL "1")
  add_compile_definitions(PIPECAT_DUAL_STREAM=1)
endif()

# Phase 6 adaptive prebuffer (NetEQ-lite): grow the playback prebuffer
# 40->80->120->160ms when PLC/FEC/RED recovery events exceed a rate threshold
# in a rolling window; decay back one step after a quiet period. DARK by
# default — set PIPECAT_ADAPTIVE_PREBUFFER=1 at configure time to enable.
if("$ENV{PIPECAT_ADAPTIVE_PREBUFFER}" STREQUAL "1")
  add_compile_definitions(PIPECAT_ADAPTIVE_PREBUFFER=1)
endif()

# Phase 5 NACK retransmit lane: on a >=3-packet seq gap (beyond RED's N-2
# reach) re-request the missing seqs over the RTVI data channel; an rtx that
# lands within 20ms is spliced as REAL audio (bit-exact — beats concealment).
# Requires the server side (pipecat-house-voice nack_retransmit.py). DARK by
# default — set PIPECAT_NACK=1 at configure time to enable.
if("$ENV{PIPECAT_NACK}" STREQUAL "1")
  add_compile_definitions(PIPECAT_NACK=1)
endif()

# Gap-resume window (ms): a FULL playback-ring drain followed by a refill
# within this window is counted as a mid-speech gap (`gap_resumes` in
# /playback/stats) instead of silently re-arming as end-of-utterance.
# Default 750ms (in media.cpp); pure instrumentation, no behavior change.
if(NOT "$ENV{PIPECAT_GAP_RESUME_MS}" STREQUAL "")
  add_compile_definitions(PIPECAT_GAP_RESUME_MS=$ENV{PIPECAT_GAP_RESUME_MS})
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
  # Tuned 2026-07-07 (live /xvf/tune sweep on kitchen, mic un-muted): 12dB gave
  # the lowest self-echo tone leak (rms 766 vs 1227 at 0dB); combined with
  # DTSENSITIVE=30 the leak drops to ~380. Was 0.0f.
  set(PIPECAT_AEC_FAR_EXTGAIN_DB_VALUE "12.0f")
endif()

# XVF3800 AGC target output level. Default 0.0045f = the XMOS/Seeed stock value
# (quiet far-field). Raise it (e.g. 0.015f) to make AGC boost far-field speech
# louder toward the wake threshold. Env: PIPECAT_AGC_DESIRED_LEVEL.
if("${PIPECAT_AGC_DESIRED_LEVEL_VALUE}" STREQUAL "")
  set(PIPECAT_AGC_DESIRED_LEVEL_VALUE "0.0045f")
endif()

# LED ring controls (ported from ESPHome respeaker_xvf3800). All optional; the
# self-test lights the ring once at boot to confirm it is XMOS-wired on this board.
if(NOT "$ENV{PIPECAT_LED_ENABLE}" STREQUAL "")
  add_compile_definitions(PIPECAT_LED_ENABLE=$ENV{PIPECAT_LED_ENABLE})
endif()
if(NOT "$ENV{PIPECAT_LED_SELFTEST}" STREQUAL "")
  add_compile_definitions(PIPECAT_LED_SELFTEST=$ENV{PIPECAT_LED_SELFTEST})
endif()
if(NOT "$ENV{PIPECAT_LED_BRIGHTNESS}" STREQUAL "")
  add_compile_definitions(PIPECAT_LED_BRIGHTNESS=$ENV{PIPECAT_LED_BRIGHTNESS})
endif()

# XVF3800 mic input gain (AUDIO_MGR MIC_GAIN). High values (90) saturate loud/
# near-field speech before the limiter acts -> clipping (peak pinned at 32768),
# which degrades the wake model. Lower it for headroom; AGC boosts far-field
# back up. Env: PIPECAT_MIC_GAIN (float, e.g. 60).
if(NOT "$ENV{PIPECAT_MIC_GAIN}" STREQUAL "")
  add_compile_definitions(PIPECAT_MIC_GAIN=$ENV{PIPECAT_MIC_GAIN})
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
