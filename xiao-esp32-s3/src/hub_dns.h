#pragma once

#include <stddef.h>
#include <stdint.h>

// Designated-resolver adapter (Satellite Hub Discovery spec v1.3 §7 item 1a,
// pass-3 B2; card t_056748fb).
//
// lwIP's getaddrinfo() returns EAI_FAIL for every non-ERR_OK and dns.c fires
// the same found-callback on a timeout and on a negative reply, so the firmware
// cannot tell "AGH said the name does not exist" from "AGH is silent". For the
// NVS override host the firmware therefore asks the DESIGNATED resolver
// (dns_servers[0], the DHCP primary = AGH .208) itself: one raw RFC 1035 A
// query over UDP, 2 tries x 2 s, and the answer is accepted only when its
// provenance checks out (source == queried server:port, QR=1, TXID match,
// QDCOUNT=1, the echoed question == the sent name/type/class, TC=0). Only then
// is the RCODE believed. The secondary resolver is NEVER asked: "AGH silent,
// then NextDNS NXDOMAIN" cannot happen inside the adapter.
//
// The wire build/parse is pure (host-tested); the socket I/O uses BSD sockets
// (lwIP on the device, libc on the host test's loopback fake resolver).

enum class HubDnsResult : uint8_t {
  kAOk = 0,     // RCODE 0 with >= 1 A RR -> *out
  kNxdomain,    // RCODE 3 from the designated server (URL-class upstream)
  kNodata,      // RCODE 0, no A RR
  kServfail,    // RCODE 2 or any other RCODE
  kTimeout,     // no accepted datagram in both tries
  kMismatch,    // (diagnostic only) a datagram was discarded this try
  kNoResolver,  // designated server unset or not RFC 1918
};

inline const char *hub_dns_result_name(HubDnsResult r) {
  switch (r) {
    case HubDnsResult::kAOk:
      return "a_ok";
    case HubDnsResult::kNxdomain:
      return "nxdomain";
    case HubDnsResult::kNodata:
      return "nodata";
    case HubDnsResult::kServfail:
      return "servfail";
    case HubDnsResult::kTimeout:
      return "timeout";
    case HubDnsResult::kMismatch:
      return "mismatch";
    case HubDnsResult::kNoResolver:
      return "no_resolver";
  }
  return "unknown";
}

struct HubDnsDiag {
  uint8_t tries = 0;      // datagrams sent
  uint8_t discarded = 0;  // datagrams received and refused (provenance)
  uint8_t rcode = 0xff;   // RCODE of the accepted datagram, 0xff = none
};

// Per-try timeout and try count (spec: 2 s x 2).
static constexpr uint32_t kHubDnsTryTimeoutMs = 2000;
static constexpr uint8_t kHubDnsTries = 2;
static constexpr size_t kHubDnsMaxName = 253;
static constexpr size_t kHubDnsBufSize = 512;

// RFC 1918 (10/8, 172.16/12, 192.168/16); `ip` in host byte order.
inline bool hub_dns_is_private_ipv4(uint32_t ip) {
  if ((ip & 0xff000000u) == 0x0a000000u)
    return true;
  if ((ip & 0xfff00000u) == 0xac100000u)
    return true;
  if ((ip & 0xffff0000u) == 0xc0a80000u)
    return true;
  return false;
}

// Build one A/IN query (RD=1, QDCOUNT=1). Returns the length, 0 on a bad
// name or a too-small buffer.
size_t hub_dns_build_query(const char *name, uint16_t txid, uint8_t *buf,
                           size_t cap);

// Parse one datagram against the query it must answer. kMismatch for any
// provenance failure (QR=0, TXID, QDCOUNT, question, TC=1, truncated bytes);
// otherwise the RCODE verdict. *out_ip (host byte order) is set on kAOk.
HubDnsResult hub_dns_parse_response(const uint8_t *buf, size_t len,
                                    uint16_t txid, const char *name,
                                    uint32_t *out_ip, uint8_t *out_rcode);

// One query to server:port (host byte order). Never kMismatch (that is
// per-datagram; the try continues until its timeout).
HubDnsResult hub_dns_query_server(const char *name, uint32_t server_ip,
                                  uint16_t server_port, uint32_t *out_ip,
                                  HubDnsDiag *diag);

#if defined(ESP_PLATFORM)
// Firmware entry: reads ESP_NETIF_DNS_MAIN of the STA netif, refuses a public
// or unset server (kNoResolver), then hub_dns_query_server(:53).
HubDnsResult pipecat_hub_dns_query(const char *name, uint32_t *out_ip,
                                   HubDnsDiag *diag);
// "a.b.c.d,e.f.g.h" of the main + backup resolver (R5); "" when none.
size_t pipecat_dns_servers_str(char *out, size_t cap);
#endif
