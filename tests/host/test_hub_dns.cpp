// Host test for the designated-resolver adapter (spec v1.3 §7 items 1a/7,
// pass-3 B2; card t_056748fb): hub_dns.cpp against a fake UDP resolver on
// loopback. Build: see run_hub_dns_tests.sh (ASan/UBSan).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "../../xiao-esp32-s3/src/hub_dns.h"

// --- fake resolver ----------------------------------------------------------
// Binds 127.0.0.1:<ephemeral>; on each query runs `respond` which may answer
// from the bound socket (designated) or from a SECOND socket (forged source).

enum class Mode {
  kA,            // RCODE 0, one A RR for the queried name
  kNxdomain,     // RCODE 3
  kNodata,       // RCODE 0, no RRs
  kServfail,     // RCODE 2
  kTruncated,    // TC=1 with RCODE 3
  kWrongTxid,    // RCODE 3 with txid+1 (then silence)
  kWrongName,    // RCODE 3 echoing a different question name
  kForgedSource, // RCODE 3 sent from another socket/port (then silence)
  kSilent,       // never answers
  kForgedThenA,  // forged NXDOMAIN first, then a real A from the server
  kCname,        // CNAME to other.name + A for other.name
};

struct Fake {
  int fd = -1;
  int forge_fd = -1;
  uint16_t port = 0;
  Mode mode = Mode::kA;
  volatile int queries = 0;
  volatile bool stop = false;
  pthread_t thread;
};

static size_t put_name(const char *name, uint8_t *b, size_t off) {
  const char *p = name;
  while (*p) {
    const char *q = p;
    while (*q && *q != '.')
      ++q;
    b[off++] = (uint8_t)(q - p);
    memcpy(b + off, p, (size_t)(q - p));
    off += (size_t)(q - p);
    p = *q ? q + 1 : q;
  }
  b[off++] = 0;
  return off;
}

// Build a response to query q (len n). Returns length.
static size_t build_response(const uint8_t *q, size_t n, Mode mode,
                             uint8_t *out) {
  // Copy header + question verbatim.
  memcpy(out, q, n);
  size_t off = n;
  out[2] = 0x81;  // QR=1 RD=1
  out[3] = 0x80;  // RA=1, RCODE 0
  out[6] = out[7] = 0;
  out[8] = out[9] = out[10] = out[11] = 0;
  auto add_rr = [&](const char *owner, uint16_t type, const uint8_t *rdata,
                    uint16_t rdlen) {
    off = put_name(owner, out, off);
    out[off++] = (uint8_t)(type >> 8);
    out[off++] = (uint8_t)type;
    out[off++] = 0;
    out[off++] = 1;  // IN
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = 10;  // TTL 10
    out[off++] = (uint8_t)(rdlen >> 8);
    out[off++] = (uint8_t)rdlen;
    memcpy(out + off, rdata, rdlen);
    off += rdlen;
    out[7]++;  // ANCOUNT
  };
  // Decode the question name for the owner.
  char qname[256];
  size_t qn = 0;
  size_t p = 12;
  while (q[p]) {
    if (qn)
      qname[qn++] = '.';
    memcpy(qname + qn, q + p + 1, q[p]);
    qn += q[p];
    p += 1 + q[p];
  }
  qname[qn] = '\0';
  const uint8_t a[4] = {192, 168, 1, 89};
  switch (mode) {
    case Mode::kA:
    case Mode::kForgedThenA:
      add_rr(qname, 1, a, 4);
      break;
    case Mode::kCname: {
      uint8_t target[64];
      size_t tl = put_name("other.name", target, 0);
      add_rr(qname, 5, target, (uint16_t)tl);
      add_rr("other.name", 1, a, 4);
      break;
    }
    case Mode::kNxdomain:
    case Mode::kWrongTxid:
    case Mode::kForgedSource:
      out[3] = 0x83;
      break;
    case Mode::kNodata:
      break;
    case Mode::kServfail:
      out[3] = 0x82;
      break;
    case Mode::kTruncated:
      out[2] = 0x83;  // QR=1 TC=1 RD=1
      out[3] = 0x83;
      break;
    case Mode::kWrongName: {
      // Rebuild the question with a different name.
      off = 12;
      off = put_name("evil.ace", out, off);
      memcpy(out + off, q + n - 4, 4);
      off += 4;
      out[3] = 0x83;
      break;
    }
    case Mode::kSilent:
      return 0;
  }
  if (mode == Mode::kWrongTxid)
    out[1] ^= 0x01;
  return off;
}

static void *fake_main(void *arg) {
  Fake *f = (Fake *)arg;
  bool forged_sent = false;
  while (!f->stop) {
    uint8_t q[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    struct timeval tv = {0, 50000};
    setsockopt(f->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int n = recvfrom(f->fd, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0)
      continue;
    f->queries++;
    uint8_t r[512];
    Mode m = f->mode;
    if (m == Mode::kForgedSource) {
      if (!forged_sent) {
        size_t rl = build_response(q, (size_t)n, m, r);
        sendto(f->forge_fd, r, rl, 0, (struct sockaddr *)&from, fl);
        forged_sent = true;
      }
      continue;  // the designated server itself stays silent
    }
    if (m == Mode::kForgedThenA) {
      size_t rl = build_response(q, (size_t)n, Mode::kNxdomain, r);
      sendto(f->forge_fd, r, rl, 0, (struct sockaddr *)&from, fl);
      usleep(20000);
      rl = build_response(q, (size_t)n, Mode::kA, r);
      sendto(f->fd, r, rl, 0, (struct sockaddr *)&from, fl);
      continue;
    }
    if (m == Mode::kWrongTxid || m == Mode::kWrongName ||
        m == Mode::kTruncated) {
      if (forged_sent)
        continue;  // one bad datagram, then silence
      forged_sent = true;
    }
    size_t rl = build_response(q, (size_t)n, m, r);
    if (rl)
      sendto(f->fd, r, rl, 0, (struct sockaddr *)&from, fl);
  }
  return nullptr;
}

static void fake_start(Fake *f, Mode mode, uint16_t want_port = 0) {
  f->mode = mode;
  f->queries = 0;
  f->stop = false;
  f->fd = socket(AF_INET, SOCK_DGRAM, 0);
  f->forge_fd = socket(AF_INET, SOCK_DGRAM, 0);
  assert(f->fd >= 0 && f->forge_fd >= 0);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(0x7f000001);
  sa.sin_port = htons(want_port);
  assert(bind(f->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
  socklen_t sl = sizeof(sa);
  assert(getsockname(f->fd, (struct sockaddr *)&sa, &sl) == 0);
  f->port = ntohs(sa.sin_port);
  assert(pthread_create(&f->thread, nullptr, fake_main, f) == 0);
}

static void fake_stop(Fake *f) {
  f->stop = true;
  pthread_join(f->thread, nullptr);
  close(f->fd);
  close(f->forge_fd);
}

static long now_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static HubDnsResult run(Mode mode, uint32_t *ip, HubDnsDiag *diag,
                        long *elapsed_ms) {
  Fake f;
  fake_start(&f, mode);
  long t0 = now_ms();
  HubDnsResult r =
      hub_dns_query_server("voice-hub-staging.ace", 0x7f000001, f.port, ip, diag);
  *elapsed_ms = now_ms() - t0;
  fake_stop(&f);
  return r;
}

// --- pure wire tests ----------------------------------------------------------

static void test_build_query() {
  uint8_t b[512];
  size_t n = hub_dns_build_query("voice-hub.ace", 0xbeef, b, sizeof(b));
  assert(n == 12 + 1 + 9 + 1 + 3 + 1 + 4);
  assert(b[0] == 0xbe && b[1] == 0xef);
  assert(b[2] == 0x01 && b[3] == 0x00);  // RD=1, QR=0
  assert(b[5] == 1);                     // QDCOUNT
  assert(memcmp(b + 12, "\x09voice-hub\x03" "ace\x00", 15) == 0);
  assert(b[n - 4] == 0 && b[n - 3] == 1 && b[n - 2] == 0 && b[n - 1] == 1);
  assert(hub_dns_build_query("", 1, b, sizeof(b)) == 0);
  assert(hub_dns_build_query("a..b", 1, b, sizeof(b)) == 0);
  assert(hub_dns_build_query("voice-hub.ace", 1, b, 20) == 0);
  // Trailing dot = same query.
  assert(hub_dns_build_query("voice-hub.ace.", 0xbeef, b, sizeof(b)) == n);
}

static void test_parse_provenance() {
  uint8_t q[512], r[512];
  size_t qn = hub_dns_build_query("voice-hub.ace", 0x1234, q, sizeof(q));
  uint32_t ip = 0;
  uint8_t rcode;
  size_t rn = build_response(q, qn, Mode::kA, r);
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kAOk);
  assert(ip == 0xc0a80159u && rcode == 0);
  // Case-insensitive echoed name.
  assert(hub_dns_parse_response(r, rn, 0x1234, "VOICE-hub.ACE", &ip, &rcode) ==
         HubDnsResult::kAOk);
  // Wrong TXID -> mismatch.
  assert(hub_dns_parse_response(r, rn, 0x1235, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kMismatch);
  // Wrong name -> mismatch.
  assert(hub_dns_parse_response(r, rn, 0x1234, "other.ace", &ip, &rcode) ==
         HubDnsResult::kMismatch);
  // QR=0 -> mismatch.
  r[2] &= 0x7f;
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kMismatch);
  r[2] |= 0x80;
  // TC=1 -> mismatch.
  r[2] |= 0x02;
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kMismatch);
  r[2] &= ~0x02;
  // QDCOUNT=2 -> mismatch.
  r[5] = 2;
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kMismatch);
  r[5] = 1;
  // Truncated bytes -> mismatch, never a crash (ASan).
  for (size_t cut = 0; cut < rn; cut++)
    assert(hub_dns_parse_response(r, cut, 0x1234, "voice-hub.ace", &ip,
                                  &rcode) == HubDnsResult::kMismatch);
  // RCODEs.
  rn = build_response(q, qn, Mode::kNxdomain, r);
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kNxdomain && rcode == 3);
  rn = build_response(q, qn, Mode::kServfail, r);
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kServfail && rcode == 2);
  rn = build_response(q, qn, Mode::kNodata, r);
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kNodata);
  rn = build_response(q, qn, Mode::kCname, r);
  ip = 0;
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kAOk && ip == 0xc0a80159u);
  // An A RR for a DIFFERENT owner (no CNAME) is not our answer -> NODATA.
  rn = build_response(q, qn, Mode::kNodata, r);
  {
    uint8_t rr[64];
    size_t off = put_name("other.name", rr, 0);
    const uint8_t tail[14] = {0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 10, 0, 0, 1};
    memcpy(rr + off, tail, 14);
    memcpy(r + rn, rr, off + 14);
    rn += off + 14;
    r[7] = 1;
  }
  assert(hub_dns_parse_response(r, rn, 0x1234, "voice-hub.ace", &ip, &rcode) ==
         HubDnsResult::kNodata);
  // Compression pointer loop -> mismatch, bounded.
  uint8_t loop[32];
  memset(loop, 0, sizeof(loop));
  loop[0] = 0x12;
  loop[1] = 0x34;
  loop[2] = 0x81;
  loop[5] = 1;
  loop[12] = 0xc0;
  loop[13] = 12;  // points at itself
  assert(hub_dns_parse_response(loop, 20, 0x1234, "voice-hub.ace", &ip,
                                &rcode) == HubDnsResult::kMismatch);
}

static void test_private_ipv4() {
  assert(hub_dns_is_private_ipv4(0xc0a801d0u));  // 192.168.1.208
  assert(hub_dns_is_private_ipv4(0x0a000001u));
  assert(hub_dns_is_private_ipv4(0xac100001u));
  assert(!hub_dns_is_private_ipv4(0x2d5a1cd7u));  // 45.90.28.215 NextDNS
  assert(!hub_dns_is_private_ipv4(0x01010101u));
}

// --- socket tests against the fake ----------------------------------------------

static void test_designated_answers() {
  uint32_t ip = 0;
  HubDnsDiag d;
  long ms;
  assert(run(Mode::kA, &ip, &d, &ms) == HubDnsResult::kAOk);
  assert(ip == 0xc0a80159u && d.tries == 1 && d.discarded == 0 && d.rcode == 0);
  assert(ms < 500);
  assert(run(Mode::kNxdomain, &ip, &d, &ms) == HubDnsResult::kNxdomain);
  assert(d.rcode == 3 && ms < 500);
  assert(run(Mode::kNodata, &ip, &d, &ms) == HubDnsResult::kNodata);
  assert(run(Mode::kServfail, &ip, &d, &ms) == HubDnsResult::kServfail);
  assert(run(Mode::kCname, &ip, &d, &ms) == HubDnsResult::kAOk);
}

static void test_silent_server_times_out_in_two_tries() {
  uint32_t ip = 0;
  HubDnsDiag d;
  long ms;
  assert(run(Mode::kSilent, &ip, &d, &ms) == HubDnsResult::kTimeout);
  assert(d.tries == 2 && d.discarded == 0 && d.rcode == 0xff);
  // 2 x 2 s, with slack: <= 4.2 s (spec), and not < 3.9 s.
  assert(ms >= 3900 && ms <= 4300);
}

static void test_forged_nxdomain_is_discarded() {
  // The forged datagram (another source port) arrives; the designated server
  // stays silent -> TIMEOUT with discarded >= 1, NEVER nxdomain.
  uint32_t ip = 0;
  HubDnsDiag d;
  long ms;
  HubDnsResult r = run(Mode::kForgedSource, &ip, &d, &ms);
  assert(r == HubDnsResult::kTimeout);
  assert(d.discarded >= 1);
  assert(r != HubDnsResult::kNxdomain);
  // Forged NXDOMAIN then the real A: the A wins, the forgery is counted.
  r = run(Mode::kForgedThenA, &ip, &d, &ms);
  assert(r == HubDnsResult::kAOk && d.discarded >= 1 && ip == 0xc0a80159u);
}

static void test_wrong_txid_name_tc_are_discarded() {
  uint32_t ip = 0;
  HubDnsDiag d;
  long ms;
  assert(run(Mode::kWrongTxid, &ip, &d, &ms) == HubDnsResult::kTimeout);
  assert(d.discarded >= 1);
  assert(run(Mode::kWrongName, &ip, &d, &ms) == HubDnsResult::kTimeout);
  assert(d.discarded >= 1);
  assert(run(Mode::kTruncated, &ip, &d, &ms) == HubDnsResult::kTimeout);
  assert(d.discarded >= 1);
}

static void test_only_the_designated_server_is_ever_asked() {
  // The adapter is handed ONE server; a second fake on another port must see
  // zero queries however the first behaves (no secondary fallback).
  // The "secondary" sits on primary.port + 1 so a mutant that walks to the
  // next server on timeout (the lwIP dns.c shape) has something to hit.
  Fake primary, secondary;
  fake_start(&primary, Mode::kSilent);
  fake_start(&secondary, Mode::kNxdomain, (uint16_t)(primary.port + 1));
  uint32_t ip = 0;
  HubDnsDiag d;
  HubDnsResult r = hub_dns_query_server("voice-hub-staging.ace", 0x7f000001,
                                        primary.port, &ip, &d);
  assert(r == HubDnsResult::kTimeout);
  assert(primary.queries == 2);
  assert(secondary.queries == 0);
  fake_stop(&primary);
  fake_stop(&secondary);
}

int main() {
  test_build_query();
  test_parse_provenance();
  test_private_ipv4();
  test_designated_answers();
  // First socket test with a silent primary: a "walk to the next server"
  // mutant must die HERE (the secondary answers), not hang a later test.
  test_only_the_designated_server_is_ever_asked();
  test_forged_nxdomain_is_discarded();
  test_wrong_txid_name_tc_are_discarded();
  test_silent_server_times_out_in_two_tries();
  printf("hub_dns: PASS\n");
  return 0;
}
