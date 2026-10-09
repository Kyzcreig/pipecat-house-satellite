// Fleet identity (Satellite Fleet v2 P4, t_54916498, PRD §5.2/§5.3): the
// room-agnostic image identifies itself by its Wi-Fi STA MAC, never by a
// baked room name. Pure, header-only, host-testable (tests/host/
// test_fleet_identity.cpp); the ESP side supplies the MAC bytes and the
// running-image sha.
//
//   request_data on POST /api/offer:  {"mac":"aa:bb:..","fw_sha256":"..",
//                                      "fw_version":"..","boot_count":N}
//   /ota/status:                       "mac", "offer_url", "label"
//   mDNS default hostname:             xvf3800-<last 3 MAC bytes hex>
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// "aa:bb:cc:dd:ee:ff" + NUL.
#define PIPECAT_MAC_STR_LEN 18
// "xvf3800-" + 6 hex + NUL.
#define PIPECAT_MAC_HOSTNAME_LEN 15

// Lowercase colon-separated MAC; the form ARP shows and the manifest stores.
static inline void pipecat_mac_format(const uint8_t mac[6], char *out,
                                      size_t capacity) {
  snprintf(out, capacity, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

// Default mDNS/AGH short name for a board with no NVS label (§5.3):
// xvf3800-<last 3 MAC bytes>, e.g. xvf3800-46c220.
static inline void pipecat_mac_hostname(const uint8_t mac[6], char *out,
                                        size_t capacity) {
  snprintf(out, capacity, "xvf3800-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

// Copy `src` into `out` as a JSON string body, escaping `"` and `\`; any
// other control byte is dropped. Returns the number of bytes written (no
// NUL). Fields come from our own firmware (sha hex, PROJECT_VER), so this is
// belt-and-braces, not a general JSON encoder.
static inline size_t pipecat_json_escape(const char *src, char *out,
                                         size_t capacity) {
  size_t n = 0;
  for (const char *p = src; p && *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20)
      continue;
    if (c == '"' || c == '\\') {
      if (n + 2 >= capacity)
        break;
      out[n++] = '\\';
      out[n++] = (char)c;
    } else {
      if (n + 1 >= capacity)
        break;
      out[n++] = (char)c;
    }
  }
  if (capacity)
    out[n < capacity ? n : capacity - 1] = '\0';
  return n;
}

// The request_data object the hub's front door cross-checks against ARP
// (G5: an existing optional offer field, no pipecat change). Writes the JSON
// object text into out; returns snprintf's count (>= capacity means
// truncated, and the caller must not send it).
static inline int pipecat_request_data_json(const char *mac_str,
                                            const char *fw_sha256,
                                            const char *fw_version,
                                            uint32_t boot_count, char *out,
                                            size_t capacity) {
  char sha[80];
  char ver[64];
  pipecat_json_escape(fw_sha256 ? fw_sha256 : "unknown", sha, sizeof(sha));
  pipecat_json_escape(fw_version ? fw_version : "unknown", ver, sizeof(ver));
  return snprintf(out, capacity,
                  "{\"mac\":\"%s\",\"fw_sha256\":\"%s\",\"fw_version\":\"%s\","
                  "\"boot_count\":%lu}",
                  mac_str, sha, ver, (unsigned long)boot_count);
}
