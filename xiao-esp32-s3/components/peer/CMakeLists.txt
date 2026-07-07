set(PEER_PROJECT_PATH "../../../esp32-s3-box-3/deps/libpeer")
file(GLOB CODES "${PEER_PROJECT_PATH}/src/*.c")
list(FILTER CODES EXCLUDE REGEX ".*/peer_connection\\.c$")

idf_component_register(
  SRCS ${CODES} "peer_connection.c"
  INCLUDE_DIRS "${PEER_PROJECT_PATH}/src"
  REQUIRES mbedtls srtp json esp_netif
)

if(NOT IDF_TARGET STREQUAL linux)
  add_definitions("-DESP32 -DCONFIG_USE_LWIP=1 -DCONFIG_AUDIO_BUFFER_SIZE=8096 -DCONFIG_DATA_BUFFER_SIZE=102400 -D__BYTE_ORDER=__LITTLE_ENDIAN")
endif()

# Force ECDSA, because aiortc doesn't want to use RSA.
# KeepAlive timeout (ms): the server sends periodic STUN binding keepalives (ICE
# consent freshness, ~5s cadence); if none arrive for this long the peer
# transitions to CLOSED and the device esp_restart()s to re-offer. Upstream
# default is 10000. It was previously 0 (disabled), which left the device stuck
# in COMPLETED forever when the server vanished — requiring a manual power-cycle.
#
# 10000 was too aggressive: on a marginal 2.4GHz link (e.g. theater roamed to a
# congested AP), transient UDP packet loss creates ~10-15s audio gaps that
# SELF-HEAL (measured: server logs audio resume with no reconnect) — but the 10s
# keepalive fired right at that boundary and rebooted a recovering connection,
# producing the ~20-85s "peer churn" that split wake turns. Raised to 30000 so
# self-healing RF blips survive; a genuinely dead server is still caught quickly
# by the PEER_CONNECTION_FAILED handler + the 30s initial-connect watchdog, and
# within 30s here as the backstop. (root-caused 2026-07-07: kitchen at -33dBm on
# a clean AP had 2.2h uptime with this at 10000; theater at -53dBm on a 64%-util
# AP churned every ~20-85s — same firmware, proving RF+keepalive, not the code.)
add_definitions("-DHTTP_DO_NOT_USE_CUSTOM_CONFIG -DMQTT_DO_NOT_USE_CUSTOM_CONFIG -DCONFIG_DTLS_USE_ECDSA=1 -DCONFIG_USE_USRSCTP=0 -DDISABLE_PEER_SIGNALING=0 -DCONFIG_KEEPALIVE_TIMEOUT=30000")
