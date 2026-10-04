"""Source contract for re-offer-without-restart (GADGET-2, t_db77e56b).

Pins the invariants the bench measurement depends on and the ones a later
edit is most likely to break: the flag is DEFAULT OFF and a flag-off build is
the live behaviour; the teardown parks the audio publisher and detaches RTVI
BEFORE peer_connection_destroy; nothing re-dials from inside a libpeer
callback; RTVI/queue/task creation is idempotent; the HTTP kick is registered
from webrtc.cpp with its own reserved slot.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
CONFIG = (SRC / "pipecat_build_config.h.in").read_text()
CMAKE = (ROOT / "xiao-esp32-s3" / "CMakeLists.txt").read_text()
MAIN = (SRC / "main.cpp").read_text()
HEADER = (SRC / "main.h").read_text()
WEBRTC = (SRC / "webrtc.cpp").read_text()
RTVI = (SRC / "rtvi.cpp").read_text()
MEDIA = (SRC / "media.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()
PEER = (ROOT / "xiao-esp32-s3" / "components" / "peer" / "peer_connection.c").read_text()


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


# --- default OFF, byte-identical live behaviour ---------------------------
assert re.search(r"#ifndef PIPECAT_REDIAL\n#define PIPECAT_REDIAL 0\n#endif", CONFIG)
assert 'if("$ENV{PIPECAT_REDIAL}" STREQUAL "1")\n  add_compile_definitions(PIPECAT_REDIAL=1)' in CMAKE
assert "PIPECAT_REDIAL=0" not in CMAKE
for path in SRC.iterdir():
    if path.suffix in (".c", ".cpp", ".h"):
        assert not re.search(r"#define PIPECAT_REDIAL 1\b", path.read_text()), path.name

# Flag-off arm of the main loop is the live reboot-to-re-offer, verbatim.
loop = MAIN[MAIN.index("while (1) {", MAIN.index("extern \"C\" void app_main")) :]
off_arm = loop[loop.index("#else", loop.index("#if PIPECAT_REDIAL")) : loop.index("#endif")]
assert "esp_restart();" in off_arm
assert "restarting to re-offer" in off_arm
on_arm = loop[loop.index("#if PIPECAT_REDIAL") : loop.index("#else")]
assert "esp_restart();" not in on_arm
assert "pipecat_webrtc_redial_tick(reconnect_deadline)" in on_arm
# An expired watchdog is pinned at its deadline; the loop must re-arm it.
assert "reconnect_watchdog = PipecatReconnectWatchdog();" in on_arm

# --- the reboot fallback survives, inside the policy ----------------------
failed = function_body(WEBRTC, "void pipecat_webrtc_note_attempt_failed(")
assert "s_redial_policy.note_failure()" in failed
assert "esp_restart();" in failed
# peer_connection_create failure (heap) also falls back to a reboot.
attempt = function_body(WEBRTC, "static void pipecat_webrtc_attempt()")
assert "esp_restart();" in attempt

# --- teardown ordering: publisher parked, RTVI detached, THEN destroy ------
teardown = function_body(WEBRTC, "static void pipecat_webrtc_teardown()")
order = [
    teardown.index("pipecat_webrtc_connected = false;"),
    teardown.index("pipecat_audio_publisher_detach();"),
    teardown.index("pipecat_rtvi_detach();"),
    teardown.index("peer_connection = NULL;"),
    teardown.index("peer_connection_destroy(old);"),
]
assert order == sorted(order), order

# Publisher handoff is the atomic pointer + in_send bracket, not a task kill.
detach = function_body(WEBRTC, "static void pipecat_audio_publisher_detach()")
assert "__atomic_store_n(&s_publisher_pc, (PeerConnection *)NULL" in detach
assert "s_publisher_in_send" in detach
assert "vTaskDelete" not in WEBRTC and "vTaskSuspend" not in WEBRTC
pub_loop = function_body(WEBRTC, "void pipecat_send_audio_task(")
assert pub_loop.index("s_publisher_in_send, true") < pub_loop.index("__atomic_load_n(&s_publisher_pc")
assert pub_loop.index("pipecat_send_audio(peer_connection);") < pub_loop.index("s_publisher_in_send, false")
# The task is created once; a re-dial only re-points it.
attach = function_body(WEBRTC, "static void pipecat_audio_publisher_attach(")
assert "if (s_audio_publisher_task != nullptr)" in attach
assert WEBRTC.count('"audio_publisher"') == 1
# media.cpp tolerates the parked (NULL) target.
assert "encoded_size > 0 && peer_connection != NULL" in MEDIA

# --- never re-dial from inside a libpeer callback -------------------------
for sig in (
    "static void pipecat_onconnectionstatechange_task(",
    "static void pipecat_ondatachannel_onclose_task(",
):
    body = function_body(WEBRTC, sig)
    assert "pipecat_redial_request(" in body, sig
    assert "pipecat_webrtc_redial(" not in body, sig
    assert "peer_connection_destroy" not in body, sig
# The lost-peer handler keeps the live contract (reconnect_watchdog test).
lost = WEBRTC[WEBRTC.index("if (state == PEER_CONNECTION_DISCONNECTED") : WEBRTC.index("} else if (state == PEER_CONNECTION_CONNECTED)")]
assert "esp_restart();" not in lost
# The tick consumes the request after peer_connection_loop() returned.
tick = function_body(WEBRTC, "bool pipecat_webrtc_redial_tick(")
assert "__atomic_exchange_n(&s_redial_requested" in tick
wl = function_body(WEBRTC, "void pipecat_webrtc_loop()")
assert "if (peer_connection == NULL)" in wl  # between attempts: no loop on NULL

# --- RTVI: idempotent init, detach nulls the pointer and the staged pong ---
init = function_body(RTVI, "void pipecat_init_rtvi(")
assert init.index("if (rtvi_queue != NULL)") < init.index("xQueueCreate(")
assert init.index("peer_connection = connection;") < init.index("if (rtvi_queue != NULL)")
det = function_body(RTVI, "void pipecat_rtvi_detach()")
assert "peer_connection = NULL;" in det and "pending_pong_ready = false;" in det
assert "if (!pending_pong_ready || peer_connection == NULL)" in RTVI

# --- hub-close signals reach the firmware ---------------------------------
assert "pipecat_ondatachannel_onclose_task);" in WEBRTC  # SCTP ABORT/SHUTDOWN
assert "MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY" in PEER        # DTLS close_notify
assert PEER[PEER.rindex("#ifdef PIPECAT_REDIAL", 0, PEER.index("MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY")) :].index("STATE_CHANGED(pc, PEER_CONNECTION_CLOSED)") > 0

# --- HTTP surface: kick + telemetry, slot reserved, ota.cpp table untouched -
assert re.search(r'\.uri = "/webrtc/redial",\s*\.method = HTTP_POST', WEBRTC)
server = function_body(OTA, "void pipecat_init_ota_server()")
assert "#if PIPECAT_REDIAL\n  config.max_uri_handlers += 1;" in server
assert "pipecat_webrtc_register_http(g_ota_server)" in server
assert '"/webrtc/redial"' not in OTA
params = function_body(OTA, "static esp_err_t xvf_params_handler(")
assert "pipecat_webrtc_redial_json(body + used" in params
assert "kParamsBodyCapacity = PIPECAT_REDIAL ? 1536 : 1024" in params
redial_json = function_body(WEBRTC, "size_t pipecat_webrtc_redial_json(")
for field in (
    "generation", "peer_state", "connected", "redials_total", "attempts_total",
    "redial_connects", "last_trigger", "last_redial_to_connected_ms",
    "consecutive_failures", "heap_free_int", "heap_min_free_int",
    "heap_largest_dma", "heap_free_psram",
):
    assert f'\\"{field}\\"' in redial_json, field
for decl in (
    "pipecat_webrtc_redial_tick", "pipecat_webrtc_note_attempt_failed",
    "pipecat_webrtc_request_redial", "pipecat_webrtc_register_http",
    "pipecat_webrtc_redial_json", "pipecat_rtvi_detach",
):
    assert decl in HEADER, decl

print("webrtc redial source contract: PASS")
