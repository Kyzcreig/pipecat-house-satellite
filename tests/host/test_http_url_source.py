#!/usr/bin/env python3
"""Source contract (spec v1.3 §7 items 3-5, pass-2 R7; t_056748fb).

Locks the hub-URL seams: exactly ONE `config.url =` in http.cpp and its RHS is
the pipecat_hub_url buffer (no second assignment can re-bake an IP); the
http.cpp status ladder maps 400/404/405 -> URL-class and 401/403/5xx/transport
-> transport-class; webrtc.cpp reports CONNECTED and never_connected; main.cpp
inits the policy right after nvs_flash_init and ticks the preflight; ota.cpp
registers GET/POST/DELETE /hub/url with three more handler slots and reports
hub_url_source + dns_servers on /ota/status; the policy header never sees an
EAI_* code or errno (classification by the ladder, pass-3 B2).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "xiao-esp32-s3" / "src"
HTTP = (SRC / "http.cpp").read_text()
WEBRTC = (SRC / "webrtc.cpp").read_text()
MAIN = (SRC / "main.cpp").read_text()
OTA = (SRC / "ota.cpp").read_text()
POLICY = (SRC / "hub_url_policy.h").read_text()
HUB = (SRC / "hub_url.cpp").read_text()
DNS = (SRC / "hub_dns.cpp").read_text()
CMAKE = (SRC / "CMakeLists.txt").read_text()


def test_single_config_url_assignment() -> None:
    assigns = re.findall(r"^\s*config\.url\s*=\s*([^;]+);", HTTP, re.M)
    assert assigns == ["url_buf"], f"config.url assignments: {assigns}"
    assert "PIPECAT_SMALLWEBRTC_URL" not in HTTP, "http.cpp must not bake the URL"
    assert re.search(r"if \(!pipecat_hub_url\(url_buf, sizeof\(url_buf\)\)\) \{", HTTP)


def test_status_ladder_classification() -> None:
    body = HTTP[HTTP.index("esp_err_t err = esp_http_client_perform(client);"):]
    url_class = re.search(
        r"status_code == 400 \|\| status_code == 404 \|\|\s*status_code == 405\) \{\s*"
        r"pipecat_hub_url_note_result\(HubUrlEvent::kHttp4xxUrl\);", body)
    assert url_class, "400/404/405 must be the ONLY URL-class statuses"
    assert re.search(r"status_code == 401 \|\| status_code == 403\) \{\s*"
                     r"pipecat_hub_url_note_result\(HubUrlEvent::kHttp403\);", body)
    assert re.search(r"status_code >= 500\) \{\s*"
                     r"pipecat_hub_url_note_result\(HubUrlEvent::kHttp5xx\);", body)
    assert re.search(r"if \(err != ESP_OK\) \{\s*"
                     r"pipecat_hub_url_note_result\(HubUrlEvent::kTransportErr\);", body)
    assert body.count("HubUrlEvent::kMalformed200") == 2, "both parse failures"


def test_session_edges_reported() -> None:
    connected = WEBRTC[WEBRTC.index("state == PEER_CONNECTION_CONNECTED"):]
    connected = connected[: connected.index("pipecat_audio_publisher_attach")]
    assert "pipecat_hub_url_note_result(HubUrlEvent::kConnected)" in connected
    tick = WEBRTC[WEBRTC.index("bool pipecat_webrtc_redial_tick"):]
    assert "pipecat_hub_url_note_result(HubUrlEvent::kNeverConnected)" in tick
    # No second PeerConnection: the probe uses the existing redial request.
    assert WEBRTC.count("static PeerConnection *peer_connection") == 1
    assert "peer_connection" not in HUB, "hub_url.cpp must never touch the peer"
    assert HUB.count("pipecat_webrtc_request_redial()") == 1


def test_init_after_nvs_and_tick_in_loop() -> None:
    init = MAIN.index("pipecat_hub_url_init();")
    assert MAIN.index("nvs_flash_init();") < init < MAIN.index("pipecat_init_webrtc();")
    loop = MAIN[MAIN.index("while (1) {"):]
    assert "pipecat_hub_url_tick();" in loop


def test_ota_surface() -> None:
    assert "config.max_uri_handlers += 3;  // GET/POST/DELETE /hub/url" in OTA
    for m in ("HTTP_GET", "HTTP_POST", "HTTP_DELETE"):
        assert re.search(rf'\.uri = "/hub/url",\s*\.method = {m},', OTA), m
    assert OTA.count("httpd_register_uri_handler(g_ota_server, &hub_url_") == 3
    status = OTA[OTA.index("static esp_err_t ota_status_handler"):]
    status = status[: status.index("static esp_err_t hub_url_reply")]
    assert "pipecat_hub_url_status_json(hub_json, sizeof(hub_json));" in status
    assert "constexpr size_t kStatusBodyCapacity = 1536;" in status
    assert re.search(r"char body\[\d+\]", status) is None, "/ota/status body must be heap (4 KB httpd stack)"
    reply = OTA[OTA.index("static esp_err_t hub_url_reply"):OTA.index("static esp_err_t hub_url_get_handler")]
    assert re.search(r"char \w+\[[^\]]+\];", reply) is None, "hub_url_reply buffers must be heap"
    post = OTA[OTA.index("static esp_err_t hub_url_post_handler"):OTA.index("static esp_err_t hub_url_delete_handler")]
    assert re.search(r"char \w+\[[^\]]+\];", post) is None, "POST body buffer must be heap"
    for key in ("hub_url_source", "url_failures", "resolver_timeouts", "dns_servers"):
        assert f'\\"{key}\\":' in HUB, key
    for key in ("source", "hub_url_source", "url_failures", "resolver_timeouts",
                "dns_servers", "nvs_url", "baked_url"):
        assert f'\\"{key}\\":' in OTA[OTA.index("static esp_err_t hub_url_reply"):], key
    # POST caps: 200-byte URL, http only (the validator), non-JSON -> 400.
    assert "#define HUB_URL_MAX_LEN 200" in (SRC / "hub_url.h").read_text()
    assert "HTTPD_400_BAD_REQUEST" in OTA[OTA.index("static esp_err_t hub_url_post_handler"):]


def test_offer_url_is_effective() -> None:
    """/ota/status offer_url == the url /hub/url reports (Apollo r1 on PR #41).

    The fleet drift `check` compares offer_url to the manifest hub_host, so a
    unit dialing its NVS override must REPORT that URL, not the baked name.
    Both handlers go through pipecat_hub_url_effective(); the baked name moves
    to `baked_url` beside it (golden guard).
    """
    status = OTA[OTA.index("static esp_err_t ota_status_handler"):]
    status = status[: status.index("static esp_err_t hub_url_reply")]
    reply = OTA[OTA.index("static esp_err_t hub_url_reply"):OTA.index("static esp_err_t hub_url_get_handler")]
    # offer_url is the escaped effective URL, immediately followed by baked_url.
    assert "pipecat_json_escape(pipecat_hub_url_effective(), offer_json, kOfferJson);" in status
    fmt = re.search(r'\\"offer_url\\":\\"%s\\",\\"baked_url\\":\\"%s\\",', status)
    assert fmt, "/ota/status must report offer_url then baked_url"
    args = re.search(r"label_json,\s*offer_json,\s*PIPECAT_SMALLWEBRTC_URL,", status)
    assert args, "offer_url arg must be offer_json (effective), baked_url PIPECAT_SMALLWEBRTC_URL"
    assert status.count("PIPECAT_SMALLWEBRTC_URL") == 1, "the baked macro feeds baked_url only"
    # ONE calloc, freed by its base pointer. a56fb07 freed the interior `body`
    # pointer and the bench panicked on the first GET (2026-10-13 05:40:32Z).
    assert status.count("calloc(") == 1
    assert "free(offer_json);" in status and "free(body)" not in status
    # /hub/url `url` is the SAME value (same accessor), never derived from the state name.
    assert "pipecat_json_escape(pipecat_hub_url_effective(), url_json, kNvsJson);" in reply
    assert re.search(r'"\{\\"url\\":\\"%s\\",[^;]*?\n\s*url_json,', reply), "/hub/url url arg must be url_json"
    assert 'strcmp(pipecat_hub_url_source_name(), "nvs")' not in reply
    # The accessor uses the policy's own dial predicate, not a string compare.
    acc = HUB[HUB.index("const char *pipecat_hub_url_effective()"):]
    acc = acc[: acc.index("uint32_t pipecat_hub_url_failures()")]
    assert "s_policy.dials_nvs() && s_nvs_url[0] != '\\0'" in acc
    assert "? s_nvs_url" in acc and ": PIPECAT_SMALLWEBRTC_URL" in acc
    dial = HUB[HUB.index("bool pipecat_hub_url(char *out, size_t cap)"):]
    dial = dial[: dial.index("HubUrlEvent ev;")]
    assert "s_policy.dials_nvs() && s_nvs_url[0] != '\\0'" in dial, "same predicate as the dialer"


def test_policy_never_sees_errno() -> None:
    code = re.sub(r"//[^\n]*", "", POLICY)  # comments may name what is banned
    for tok in ("EAI_", "errno", "getaddrinfo", "h_errno"):
        assert tok not in code, f"policy header must not classify by {tok}"
    # The adapter asks the designated server only; no secondary, no getaddrinfo.
    assert "getaddrinfo" not in DNS
    assert "ESP_NETIF_DNS_BACKUP" not in DNS.split("size_t pipecat_dns_servers_str")[0]
    assert "ESP_NETIF_DNS_MAIN" in DNS
    assert "hub_dns_is_private_ipv4(server)" in DNS


def test_sources_compiled() -> None:
    base = re.search(r"set\(TARGET_SRC ([^)]*)\)", CMAKE).group(1)
    assert '"hub_url.cpp"' in base and '"hub_dns.cpp"' in base, "hub sources unconditional"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("http url source contract: PASS")
