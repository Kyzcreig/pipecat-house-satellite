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

# Uplink RFC 2198 RED is wire-incompatible with an unpatched aiortc receiver,
# so it is an explicit build gate. Deploy/enable the server receive patch first.
if("$ENV{PIPECAT_UPLINK_RED}" STREQUAL "1")
  add_compile_definitions(PIPECAT_UPLINK_RED=1)
endif()
if(NOT "$ENV{PIPECAT_UPLINK_RED_DISTANCE}" STREQUAL "")
  add_compile_definitions(
    PIPECAT_UPLINK_RED_DISTANCE=$ENV{PIPECAT_UPLINK_RED_DISTANCE})
endif()

# Uplink Opus in-band FEC (t_109a4778). Opus LBRR lives only in the SILK layer,
# so INBAND_FEC=1 pins the uplink encoder to SILK-WB; =0 lets it pick CELT
# (measured +11 dB fricative-band segSNR at 64k/lane). Server-side PLC in
# uplink_loss_protection.py covers gaps either way. Default (unset) = 1 = live
# behaviour; set PIPECAT_UPLINK_INBAND_FEC=0 at configure time to build the
# CELT arm for the paired-corpus WER A/B. Ships only on a WER verdict.
if("$ENV{PIPECAT_UPLINK_INBAND_FEC}" STREQUAL "0")
  add_compile_definitions(PIPECAT_UPLINK_INBAND_FEC=0)
endif()

# Merged-but-never-flashed features (t_4f8fe707): DEFAULT OFF so a build from
# main reproduces the live firmware. Defaults live in pipecat_build_config.h.in;
# src/CMakeLists.txt drops their sources when off. Each needs its own card.
foreach(_pipecat_flag PIPECAT_UPLINK_64K PIPECAT_DECIM_COMP PIPECAT_FEC_LBRR
                      PIPECAT_XVF_AEC_FILTER)
  if("$ENV{${_pipecat_flag}}" STREQUAL "1")
    add_compile_definitions(${_pipecat_flag}=1)
  endif()
endforeach()

# Phase 6 adaptive prebuffer (NetEQ-lite): grow the playback prebuffer
# 40->80->120->160ms when PLC/FEC/RED recovery events exceed a rate threshold
# in a rolling window; decay back one step after a quiet period. DARK by
# default — set PIPECAT_ADAPTIVE_PREBUFFER=1 at configure time to enable.
# Downlink arrival/drain trace (t_5faf78b4): records per-packet service time,
# queued-behind flag, seq gaps and ring drain/refill edges on the device
# clock; GET /playback/stats?trace_from=N. Diagnostic only, default OFF.
if("$ENV{PIPECAT_ARRIVAL_TRACE}" STREQUAL "1")
  add_compile_definitions(PIPECAT_ARRIVAL_TRACE=1)
endif()

if("$ENV{PIPECAT_ADAPTIVE_PREBUFFER}" STREQUAL "1")
  add_compile_definitions(PIPECAT_ADAPTIVE_PREBUFFER=1)
endif()

# Phase 5 NACK retransmit lane: on a >=3-packet seq gap (beyond RED's N-2
# reach) request missing seqs over reliable RTVI; binary rtx replies arrive on
# pipecat-rtx (DCEP 0x81, unordered, zero-retransmit) and splice within 60ms.
# Requires the server side (pipecat-house-voice nack_retransmit.py). DARK by
# default — set PIPECAT_NACK=1 at configure time to enable.
if("$ENV{PIPECAT_NACK}" STREQUAL "1")
  add_compile_definitions(PIPECAT_NACK=1)
endif()

# GADGET-3 (t_2a5f2312): Muse-pattern fast Wi-Fi re-join. On a link loss,
# reconnect at once to the cached BSSID on its channel (WIFI_FAST_SCAN, no
# backoff), refusing it below PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR dBm (-75);
# one failure falls back to all-channel by-signal with the 1 s backoff. Measured
# 2026-10-04 on the kitchen (23 UniFi kicks, device clock): median 1574 ms, p95
# 1612 ms, near AP 23/23 vs 2556 ms on the stock path -> PASS (docs/decisions/
# 2026-10-04-gadget3-wifi-fast-rejoin.md in pipecat-house-voice). Default OFF
# like PIPECAT_DUAL_STREAM: the golden build sets PIPECAT_WIFI_FAST_REJOIN=1 in
# its build environment (golden bump, Apollo GO), so the manifest records it.
if("$ENV{PIPECAT_WIFI_FAST_REJOIN}" STREQUAL "1")
  add_compile_definitions(PIPECAT_WIFI_FAST_REJOIN=1)
endif()
if(NOT "$ENV{PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR}" STREQUAL "")
  add_compile_definitions(
    PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR=$ENV{PIPECAT_WIFI_FAST_REJOIN_RSSI_FLOOR})
endif()

# Gap-resume window (ms): a FULL playback-ring drain followed by a refill
# within this window is counted as a mid-speech gap (`gap_resumes` in
# /playback/stats) instead of silently re-arming as end-of-utterance.
# Default 750ms (in media.cpp); pure instrumentation, no behavior change.
if(NOT "$ENV{PIPECAT_GAP_RESUME_MS}" STREQUAL "")
  add_compile_definitions(PIPECAT_GAP_RESUME_MS=$ENV{PIPECAT_GAP_RESUME_MS})
endif()

# Re-offer without esp_restart() (GADGET-2, t_db77e56b): on a hub-closed peer,
# stale heartbeat or POST /webrtc/redial, tear down + recreate the libpeer
# PeerConnection and POST a fresh offer; XVF/I2S/LED/mdns/OTA stay up.
# esp_restart() remains the fallback after 40 failed re-dials. Also reaches
# components/peer (DTLS close_notify -> CLOSED). DEFAULT OFF; set
# PIPECAT_REDIAL=1 at configure time.
if("$ENV{PIPECAT_REDIAL}" STREQUAL "1")
  add_compile_definitions(PIPECAT_REDIAL=1)
endif()

set(WIFI_SSID_VALUE "$ENV{WIFI_SSID}")
set(WIFI_PASSWORD_VALUE "$ENV{WIFI_PASSWORD}")
set(PIPECAT_SMALLWEBRTC_URL_VALUE "$ENV{PIPECAT_SMALLWEBRTC_URL}")
set(PIPECAT_SATELLITE_ID_VALUE "$ENV{PIPECAT_SATELLITE_ID}")
set(PIPECAT_MDNS_HOSTNAME_VALUE "$ENV{PIPECAT_MDNS_HOSTNAME}")
set(PIPECAT_MDNS_INSTANCE_VALUE "$ENV{PIPECAT_MDNS_INSTANCE}")
set(PIPECAT_AEC_FAR_EXTGAIN_DB_VALUE "$ENV{PIPECAT_AEC_FAR_EXTGAIN_DB}")
set(PIPECAT_AGC_DESIRED_LEVEL_VALUE "$ENV{PIPECAT_AGC_DESIRED_LEVEL}")

# Unset id -> "scratch". "bench" is a REAL satellite key since 2026-09-30 (the board
# formerly keyed "theater", t_b5299572); an unconfigured build must not impersonate it.
if("${PIPECAT_SATELLITE_ID_VALUE}" STREQUAL "")
  set(PIPECAT_SATELLITE_ID_VALUE "scratch")
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

# Firmware build identity (t_9d8fad45): the full source commit + dirty flag,
# baked at configure time and reported on GET /xvf/params so "which firmware is
# live" is a device fact. PIPECAT_BUILD_GIT_SHA overrides (CI/archive rebuilds).
# git -C works in linked worktrees; no git at all yields "unknown", never a guess.
set(PIPECAT_BUILD_GIT_SHA_VALUE "$ENV{PIPECAT_BUILD_GIT_SHA}")
set(PIPECAT_BUILD_GIT_DIRTY_VALUE 0)
if("${PIPECAT_BUILD_GIT_SHA_VALUE}" STREQUAL "")
  execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE PIPECAT_BUILD_GIT_SHA_VALUE
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _pipecat_git_rc)
  if(NOT _pipecat_git_rc EQUAL 0 OR "${PIPECAT_BUILD_GIT_SHA_VALUE}" STREQUAL "")
    set(PIPECAT_BUILD_GIT_SHA_VALUE "unknown")
  else()
    execute_process(
      COMMAND git -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=no
      OUTPUT_VARIABLE _pipecat_git_status
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT "${_pipecat_git_status}" STREQUAL "")
      set(PIPECAT_BUILD_GIT_DIRTY_VALUE 1)
    endif()
  endif()
endif()
message(STATUS "pipecat build identity: ${PIPECAT_BUILD_GIT_SHA_VALUE} dirty=${PIPECAT_BUILD_GIT_DIRTY_VALUE}")

foreach(secret_var
    WIFI_SSID_VALUE
    WIFI_PASSWORD_VALUE
    PIPECAT_SMALLWEBRTC_URL_VALUE
    PIPECAT_SATELLITE_ID_VALUE
    PIPECAT_MDNS_HOSTNAME_VALUE
    PIPECAT_MDNS_INSTANCE_VALUE
    PIPECAT_BUILD_GIT_SHA_VALUE)
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
