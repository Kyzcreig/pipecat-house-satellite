// Designated-resolver adapter: see hub_dns.h. Pure wire build/parse up top
// (host-tested against a loopback fake resolver), BSD-socket I/O below.
#include "hub_dns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>

#include "main.h"
#else
#include <stdlib.h>
#endif

namespace {

constexpr size_t kHeaderLen = 12;

// Case-insensitive ASCII compare of two labels/names.
bool name_eq_ci(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = (char)(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
}

// Write `name` as DNS labels at buf[off]; returns the new offset, 0 on error.
size_t put_name(const char *name, uint8_t *buf, size_t off, size_t cap) {
  size_t len = strlen(name);
  if (len == 0 || len > kHubDnsMaxName)
    return 0;
  if (name[len - 1] == '.')
    len--;  // a trailing dot is the root, not an empty label
  if (len == 0)
    return 0;
  size_t i = 0;
  while (i <= len) {
    size_t j = i;
    while (j < len && name[j] != '.')
      ++j;
    size_t ll = j - i;
    if (ll == 0 || ll > 63)
      return 0;
    if (off + 1 + ll >= cap)
      return 0;
    buf[off++] = (uint8_t)ll;
    memcpy(buf + off, name + i, ll);
    off += ll;
    i = j + 1;
  }
  if (off + 1 > cap)
    return 0;
  buf[off++] = 0;
  return off;
}

// Read a (possibly compressed) name at buf[off] into out (dotted, no trailing
// dot). Returns the offset just past the name in the ORIGINAL stream, 0 on a
// malformed name. Bounded pointer chasing.
size_t get_name(const uint8_t *buf, size_t len, size_t off, char *out,
                size_t out_cap) {
  size_t out_n = 0;
  size_t end = 0;  // offset after the first pointer (the stream position)
  int hops = 0;
  while (true) {
    if (off >= len)
      return 0;
    uint8_t l = buf[off];
    if ((l & 0xc0) == 0xc0) {
      if (off + 1 >= len || ++hops > 16)
        return 0;
      size_t ptr = ((size_t)(l & 0x3f) << 8) | buf[off + 1];
      if (end == 0)
        end = off + 2;
      if (ptr >= off)
        return 0;  // forward pointers are never valid
      off = ptr;
      continue;
    }
    if (l & 0xc0)
      return 0;
    ++off;
    if (l == 0)
      break;
    if (off + l > len || l > 63)
      return 0;
    if (out_n + l + 1 >= out_cap)
      return 0;
    if (out_n)
      out[out_n++] = '.';
    memcpy(out + out_n, buf + off, l);
    out_n += l;
    off += l;
  }
  out[out_n] = '\0';
  return end ? end : off;
}

uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

size_t query_name_len(const char *name) {
  size_t len = strlen(name);
  if (len && name[len - 1] == '.')
    len--;
  return len;
}

}  // namespace

size_t hub_dns_build_query(const char *name, uint16_t txid, uint8_t *buf,
                           size_t cap) {
  if (buf == nullptr || cap < kHeaderLen + 1 + 4)
    return 0;
  memset(buf, 0, kHeaderLen);
  buf[0] = (uint8_t)(txid >> 8);
  buf[1] = (uint8_t)(txid & 0xff);
  buf[2] = 0x01;  // QR=0 OPCODE=0 AA=0 TC=0 RD=1
  buf[3] = 0x00;
  buf[5] = 1;  // QDCOUNT
  size_t off = put_name(name, buf, kHeaderLen, cap);
  if (off == 0 || off + 4 > cap)
    return 0;
  buf[off++] = 0;
  buf[off++] = 1;  // QTYPE A
  buf[off++] = 0;
  buf[off++] = 1;  // QCLASS IN
  return off;
}

HubDnsResult hub_dns_parse_response(const uint8_t *buf, size_t len,
                                    uint16_t txid, const char *name,
                                    uint32_t *out_ip, uint8_t *out_rcode) {
  if (out_rcode)
    *out_rcode = 0xff;
  if (buf == nullptr || len < kHeaderLen)
    return HubDnsResult::kMismatch;
  if (rd16(buf) != txid)
    return HubDnsResult::kMismatch;
  const uint8_t flags_hi = buf[2];
  const uint8_t flags_lo = buf[3];
  if ((flags_hi & 0x80) == 0)
    return HubDnsResult::kMismatch;  // QR=0: a query, not an answer
  if (flags_hi & 0x02)
    return HubDnsResult::kMismatch;  // TC=1
  if (rd16(buf + 4) != 1)
    return HubDnsResult::kMismatch;  // QDCOUNT != 1
  const uint16_t ancount = rd16(buf + 6);

  // The echoed question must be OUR name / A / IN.
  char qname[kHubDnsMaxName + 1];
  size_t off = get_name(buf, len, kHeaderLen, qname, sizeof(qname));
  if (off == 0 || off + 4 > len)
    return HubDnsResult::kMismatch;
  const size_t want_len = query_name_len(name);
  if (strlen(qname) != want_len || !name_eq_ci(qname, name, want_len))
    return HubDnsResult::kMismatch;
  if (rd16(buf + off) != 1 || rd16(buf + off + 2) != 1)
    return HubDnsResult::kMismatch;
  off += 4;

  const uint8_t rcode = flags_lo & 0x0f;
  if (out_rcode)
    *out_rcode = rcode;
  if (rcode == 3)
    return HubDnsResult::kNxdomain;
  if (rcode != 0)
    return HubDnsResult::kServfail;

  // RCODE 0: look for an A RR in the answer section. The owner may be our
  // name or a CNAME target within the same message; the hub's resolver
  // (AGH rewrite) answers with a direct A, so we accept any A in the answer
  // section whose owner we can parse, following the first CNAME chain found.
  char owner[kHubDnsMaxName + 1];
  char want[kHubDnsMaxName + 1];
  if (want_len >= sizeof(want))
    return HubDnsResult::kMismatch;
  memcpy(want, name, want_len);
  want[want_len] = '\0';
  for (uint16_t i = 0; i < ancount; ++i) {
    off = get_name(buf, len, off, owner, sizeof(owner));
    if (off == 0 || off + 10 > len)
      return HubDnsResult::kMismatch;
    const uint16_t type = rd16(buf + off);
    const uint16_t cls = rd16(buf + off + 2);
    const uint16_t rdlen = rd16(buf + off + 8);
    off += 10;
    if (off + rdlen > len)
      return HubDnsResult::kMismatch;
    const bool owner_ok =
        strlen(owner) == strlen(want) && name_eq_ci(owner, want, strlen(want));
    if (cls == 1 && owner_ok && type == 5) {  // CNAME: chase within message
      char target[kHubDnsMaxName + 1];
      if (get_name(buf, len, off, target, sizeof(target)) == 0)
        return HubDnsResult::kMismatch;
      strncpy(want, target, sizeof(want) - 1);
      want[sizeof(want) - 1] = '\0';
    } else if (cls == 1 && owner_ok && type == 1 && rdlen == 4) {
      if (out_ip)
        *out_ip = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                  ((uint32_t)buf[off + 2] << 8) | buf[off + 3];
      return HubDnsResult::kAOk;
    }
    off += rdlen;
  }
  return HubDnsResult::kNodata;
}

static uint16_t hub_dns_txid() {
#if defined(ESP_PLATFORM)
  return (uint16_t)esp_random();
#else
  return (uint16_t)random();
#endif
}

HubDnsResult hub_dns_query_server(const char *name, uint32_t server_ip,
                                  uint16_t server_port, uint32_t *out_ip,
                                  HubDnsDiag *diag) {
  HubDnsDiag local;
  if (diag == nullptr)
    diag = &local;
  *diag = HubDnsDiag();

  uint8_t q[kHubDnsBufSize];
  const uint16_t txid = hub_dns_txid();
  const size_t qlen = hub_dns_build_query(name, txid, q, sizeof(q));
  if (qlen == 0)
    return HubDnsResult::kTimeout;  // unencodable name: transport-class

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return HubDnsResult::kTimeout;
  struct sockaddr_in server;
  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_port = htons(server_port);
  server.sin_addr.s_addr = htonl(server_ip);

  HubDnsResult verdict = HubDnsResult::kTimeout;
  for (uint8_t t = 0; t < kHubDnsTries && verdict == HubDnsResult::kTimeout;
       ++t) {
    diag->tries++;
    if (sendto(fd, q, qlen, 0, (struct sockaddr *)&server, sizeof(server)) < 0)
      continue;
    // Wall-clock budget per try, re-armed after each discarded datagram.
    uint32_t remaining_ms = kHubDnsTryTimeoutMs;
    while (remaining_ms > 0) {
      struct timeval tv;
      tv.tv_sec = remaining_ms / 1000;
      tv.tv_usec = (remaining_ms % 1000) * 1000;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      uint8_t r[kHubDnsBufSize];
      struct sockaddr_in from;
      socklen_t from_len = sizeof(from);
      struct timeval before;
      gettimeofday(&before, nullptr);
      int n =
          recvfrom(fd, r, sizeof(r), 0, (struct sockaddr *)&from, &from_len);
      struct timeval after;
      gettimeofday(&after, nullptr);
      const long spent_ms = (after.tv_sec - before.tv_sec) * 1000L +
                            (after.tv_usec - before.tv_usec) / 1000L;
      remaining_ms =
          spent_ms >= (long)remaining_ms
              ? 0
              : remaining_ms - (uint32_t)(spent_ms < 0 ? 0 : spent_ms);
      if (n < 0)
        break;  // timeout (or socket error): next try
      // Provenance 1: the datagram must come from the queried server:port.
      if (from.sin_family != AF_INET || from.sin_port != server.sin_port ||
          from.sin_addr.s_addr != server.sin_addr.s_addr) {
        diag->discarded++;
        continue;
      }
      uint8_t rcode = 0xff;
      HubDnsResult v =
          hub_dns_parse_response(r, (size_t)n, txid, name, out_ip, &rcode);
      if (v == HubDnsResult::kMismatch) {
        diag->discarded++;
        continue;
      }
      diag->rcode = rcode;
      verdict = v;
      break;
    }
  }
  close(fd);
  return verdict;
}

#if defined(ESP_PLATFORM)
static esp_netif_t *hub_dns_sta_netif() {
  return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

HubDnsResult pipecat_hub_dns_query(const char *name, uint32_t *out_ip,
                                   HubDnsDiag *diag) {
  esp_netif_t *netif = hub_dns_sta_netif();
  esp_netif_dns_info_t dns;
  memset(&dns, 0, sizeof(dns));
  if (netif == nullptr ||
      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
      dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
    if (diag)
      *diag = HubDnsDiag();
    return HubDnsResult::kNoResolver;
  }
  const uint32_t server = ntohl(dns.ip.u_addr.ip4.addr);
  if (!hub_dns_is_private_ipv4(server)) {
    // A public designated resolver can never produce a URL-class verdict.
    if (diag)
      *diag = HubDnsDiag();
    return HubDnsResult::kNoResolver;
  }
  return hub_dns_query_server(name, server, 53, out_ip, diag);
}

size_t pipecat_dns_servers_str(char *out, size_t cap) {
  if (cap == 0)
    return 0;
  out[0] = '\0';
  esp_netif_t *netif = hub_dns_sta_netif();
  if (netif == nullptr)
    return 0;
  size_t n = 0;
  const esp_netif_dns_type_t types[] = {ESP_NETIF_DNS_MAIN,
                                        ESP_NETIF_DNS_BACKUP};
  for (size_t i = 0; i < 2; ++i) {
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    if (esp_netif_get_dns_info(netif, types[i], &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0)
      continue;
    char ip[16];
    esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, ip, sizeof(ip));
    int w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", ip);
    if (w < 0 || (size_t)w >= cap - n)
      break;
    n += (size_t)w;
  }
  return n;
}
#endif
