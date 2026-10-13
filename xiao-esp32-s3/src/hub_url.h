#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hub_url_policy.h"

// Hub URL runtime (spec v1.3 §7 item 2; card t_056748fb): NVS namespace
// `hub`, key `url`; the policy (hub_url_policy.h); the designated-resolver
// adapter (hub_dns.h) for the override host; the baked_fallback preflight.
// Firmware-only (FreeRTOS/IDF); the policy and the adapter wire code are the
// host-tested parts.

#define HUB_URL_NVS_NAMESPACE "hub"
#define HUB_URL_NVS_KEY "url"
// POST body cap (bytes, excluding NUL). NVS str values are <= 4000 bytes, the
// /ota/status body has a fixed budget; 200 is the spec's cap.
#define HUB_URL_MAX_LEN 200

// Boot: load NVS (malformed values are ignored at read time, I-2) and derive
// the initial state. Call after nvs_flash_init(), before the first dial.
void pipecat_hub_url_init();

// The URL the NEXT dial must use, written into out (IP-literal substituted
// for the override host). Returns false when this round must NOT dial (the
// override host did not resolve to a usable A answer); the policy event has
// already been recorded. The baked URL is returned as-is: its name resolves
// through lwIP getaddrinfo inside esp_http_client and every failure there is
// transport-class by construction (state baked never counts anything).
bool pipecat_hub_url(char *out, size_t cap);

// Feed a dial / session outcome (http.cpp, webrtc.cpp) or an operator event.
void pipecat_hub_url_note_result(HubUrlEvent e);

// Main-loop tick: runs the baked_fallback preflight on its interval (in a
// short-lived task so the healthy baked session is never stalled) and
// consumes its verdict. Call from the transport loop every TICK_INTERVAL.
void pipecat_hub_url_tick();

// Operator surface (ota.cpp registers the handlers; +3 max_uri_handlers).
// POST: validates (hub_url_valid, <= HUB_URL_MAX_LEN, http only), writes NVS,
// records `post` (-> redial). DELETE / POST {"url":""}: erases, records
// `erase` (-> redial). Returns ESP_OK / ESP_ERR_INVALID_ARG / an NVS error.
int pipecat_hub_url_set(const char *url);
int pipecat_hub_url_erase();

// Introspection for GET /hub/url and /ota/status.
const char *pipecat_hub_url_source_name();  // baked|nvs|baked_fallback|re-probing
const char *pipecat_hub_url_nvs();           // "" when unset
const char *pipecat_hub_url_baked();
uint32_t pipecat_hub_url_failures();
uint32_t pipecat_hub_url_resolver_timeouts();
uint32_t pipecat_hub_url_cutovers_failed();
// `"hub_url_source":"...","url_failures":N,"resolver_timeouts":N,
// "dns_servers":"a,b"` fragment (no braces, no trailing comma).
int pipecat_hub_url_status_json(char *out, size_t cap);
