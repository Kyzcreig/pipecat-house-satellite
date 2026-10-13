#pragma once

#include <stdint.h>

// Hub URL selection policy (Satellite Hub Discovery spec v1.3 §4.2 Option B,
// B-1..B-3; card t_056748fb). Pure, no FreeRTOS/IDF, host-tested by
// tests/host/test_hub_url_policy.cpp (+ the mutation arms in
// tests/host/run_hub_url_tests.sh).
//
// The satellite dials the BAKED offer URL (a `.ace` name) unless NVS `hub/url`
// holds an override. An override that the designated resolver (AGH, .208)
// says does NOT exist, or whose path the hub rejects (400/404/405), is a
// URL-class failure; three of those park the unit on the baked URL
// (`baked_fallback`, sticky, operator-visible, NVS key kept). Everything else
// (resolver silent/SERVFAIL/NODATA, refused, timeouts, 401/403, 5xx, a bad
// 200 body, never CONNECTED) is TRANSPORT-class and never counts: a staging
// hub restart or an AGH outage must not cross an override unit over to
// production (pass-1 R1 ii, pass-2 R6, pass-3 B2).
//
// From `baked_fallback` a 5-min side-effect-free preflight (adapter resolve +
// GET /health on the override) gates ONE deliberate cutover (`re-probing`:
// exactly one redial on the override); a failed cutover goes straight back to
// `baked_fallback` and the ladder's next dial is baked again (pass-3 R6).

enum class HubUrlState : uint8_t {
  kBaked = 0,        // NVS empty: dial the baked URL
  kNvs,              // NVS override in use
  kBakedFallback,    // override demoted (sticky); dial baked; preflight timer
  kReprobing,        // one cutover dial on the override in flight
};

enum class HubUrlEvent : uint8_t {
  kConnected = 0,
  // URL-class (count toward url_failures)
  kResolverNxdomain,  // correlated NXDOMAIN from the designated resolver
  kHttp4xxUrl,        // 400 / 404 / 405 from the override path
  // transport-class (never count)
  kResolverTimeout,   // no accepted datagram (incl. mismatch / no resolver)
  kResolverServfail,  // RCODE 2 (or any other non-0/3 RCODE)
  kResolverNodata,    // RCODE 0, no A RR
  kTransportErr,      // refused / connect / read timeout / baked getaddrinfo
  kHttp403,           // 401 / 403
  kHttp5xx,           // 5xx incl. 503 worker_unavailable
  kMalformed200,      // 200 with an unparseable answer
  kNeverConnected,    // answer received, kConnectTimeoutMs expired
  // operator
  kPost,   // valid POST /hub/url
  kErase,  // DELETE /hub/url or POST {"url": ""}
  // probe (5-min timer while baked_fallback)
  kPreflightOk,
  kPreflightFail,
};

inline const char *hub_url_state_name(HubUrlState s) {
  switch (s) {
    case HubUrlState::kBaked:
      return "baked";
    case HubUrlState::kNvs:
      return "nvs";
    case HubUrlState::kBakedFallback:
      return "baked_fallback";
    case HubUrlState::kReprobing:
      return "re-probing";
  }
  return "unknown";
}

inline const char *hub_url_event_name(HubUrlEvent e) {
  switch (e) {
    case HubUrlEvent::kConnected:
      return "connected";
    case HubUrlEvent::kResolverNxdomain:
      return "resolver_nxdomain";
    case HubUrlEvent::kHttp4xxUrl:
      return "http_4xx_url";
    case HubUrlEvent::kResolverTimeout:
      return "resolver_timeout";
    case HubUrlEvent::kResolverServfail:
      return "resolver_servfail";
    case HubUrlEvent::kResolverNodata:
      return "resolver_nodata";
    case HubUrlEvent::kTransportErr:
      return "transport_err";
    case HubUrlEvent::kHttp403:
      return "http_403";
    case HubUrlEvent::kHttp5xx:
      return "http_5xx";
    case HubUrlEvent::kMalformed200:
      return "malformed_200";
    case HubUrlEvent::kNeverConnected:
      return "never_connected";
    case HubUrlEvent::kPost:
      return "post";
    case HubUrlEvent::kErase:
      return "erase";
    case HubUrlEvent::kPreflightOk:
      return "preflight_ok";
    case HubUrlEvent::kPreflightFail:
      return "preflight_fail";
  }
  return "unknown";
}

// URL-class = the ONLY events that count toward url_failures. Classification
// is by the ladder (which event the adapter / http.cpp produced), never by an
// errno or EAI_* code (pass-3 B2).
inline bool hub_url_event_is_url_class(HubUrlEvent e) {
  return e == HubUrlEvent::kResolverNxdomain || e == HubUrlEvent::kHttp4xxUrl;
}

inline bool hub_url_event_is_transport_class(HubUrlEvent e) {
  switch (e) {
    case HubUrlEvent::kResolverTimeout:
    case HubUrlEvent::kResolverServfail:
    case HubUrlEvent::kResolverNodata:
    case HubUrlEvent::kTransportErr:
    case HubUrlEvent::kHttp403:
    case HubUrlEvent::kHttp5xx:
    case HubUrlEvent::kMalformed200:
    case HubUrlEvent::kNeverConnected:
      return true;
    default:
      return false;
  }
}

class HubUrlPolicy {
 public:
  // URL-class failures on the override before baked_fallback.
  static constexpr uint32_t kUrlFailureThreshold = 3;
  // Preflight cadence while baked_fallback; backs off after
  // kCutoverBackoffAfter consecutive failed cutovers (reset by POST/DELETE/
  // reboot).
  static constexpr uint32_t kPreflightIntervalMs = 5 * 60 * 1000;
  static constexpr uint32_t kPreflightBackoffMs = 30 * 60 * 1000;
  static constexpr uint32_t kCutoverBackoffAfter = 3;

  // Boot (and the `reboot` column of the table): the state is derived from
  // whether NVS holds an override; every counter is RAM-only and starts at 0.
  void init(bool nvs_present) {
    state_ = nvs_present ? HubUrlState::kNvs : HubUrlState::kBaked;
    url_failures_ = 0;
    cutovers_failed_ = 0;
    resolver_timeouts_ = 0;
    transport_events_ = 0;
  }

  // Feed one event; returns true when the caller must request a redial
  // (POST/DELETE always; preflight_ok exactly once per interval).
  bool note(HubUrlEvent e) {
    if (e == HubUrlEvent::kResolverTimeout)
      resolver_timeouts_++;
    if (hub_url_event_is_transport_class(e))
      transport_events_++;
    switch (e) {
      case HubUrlEvent::kPost:
        state_ = HubUrlState::kNvs;
        url_failures_ = 0;
        cutovers_failed_ = 0;
        return true;
      case HubUrlEvent::kErase:
        state_ = HubUrlState::kBaked;
        url_failures_ = 0;
        cutovers_failed_ = 0;
        return true;
      default:
        break;
    }
    switch (state_) {
      case HubUrlState::kBaked:
        // Nothing to fall back from; every outcome walks the ordinary ladder.
        return false;
      case HubUrlState::kNvs:
        if (e == HubUrlEvent::kConnected) {
          url_failures_ = 0;
        } else if (hub_url_event_is_url_class(e)) {
          url_failures_++;
          if (url_failures_ >= kUrlFailureThreshold)
            state_ = HubUrlState::kBakedFallback;  // fallback branch
        }
        // transport-class: failures unchanged, ladder continues on the nvs URL
        return false;
      case HubUrlState::kBakedFallback:
        if (e == HubUrlEvent::kPreflightOk) {
          state_ = HubUrlState::kReprobing;
          return true;  // the ONE cutover dial of this interval
        }
        // preflight_fail / connected / any dial outcome on baked: stay
        return false;
      case HubUrlState::kReprobing:
        if (e == HubUrlEvent::kConnected) {
          state_ = HubUrlState::kNvs;
          url_failures_ = 0;
          cutovers_failed_ = 0;
        } else if (hub_url_event_is_url_class(e) ||
                   hub_url_event_is_transport_class(e)) {
          state_ = HubUrlState::kBakedFallback;
          cutovers_failed_++;
        }
        // preflight events while a probe is in flight: ignored (one cutover)
        return false;
    }
    return false;
  }

  HubUrlState state() const {
    return state_;
  }
  // Which URL the NEXT dial uses.
  bool dials_nvs() const {
    return state_ == HubUrlState::kNvs || state_ == HubUrlState::kReprobing;
  }
  // The preflight timer runs only here.
  bool preflight_armed() const {
    return state_ == HubUrlState::kBakedFallback;
  }
  uint32_t preflight_interval_ms() const {
    return cutovers_failed_ >= kCutoverBackoffAfter ? kPreflightBackoffMs
                                                    : kPreflightIntervalMs;
  }
  uint32_t url_failures() const {
    return url_failures_;
  }
  uint32_t cutovers_failed() const {
    return cutovers_failed_;
  }
  uint32_t resolver_timeouts() const {
    return resolver_timeouts_;
  }
  uint32_t transport_events() const {
    return transport_events_;
  }

 private:
  HubUrlState state_ = HubUrlState::kBaked;
  uint32_t url_failures_ = 0;
  uint32_t cutovers_failed_ = 0;
  uint32_t resolver_timeouts_ = 0;
  uint32_t transport_events_ = 0;
};

// URL validator for POST /hub/url (B-2): `http://host[:port]/path`, ≤ cap
// bytes, no userinfo, no spaces, no control bytes. https is refused (no TLS
// on the hub). Pure; shared by the firmware handler and the host test.
inline bool hub_url_valid(const char *url, uint32_t cap) {
  if (url == nullptr)
    return false;
  uint32_t n = 0;
  for (const char *p = url; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\')
      return false;
    if (++n > cap)
      return false;
  }
  const char *prefix = "http://";
  const char *p = url;
  for (const char *q = prefix; *q; ++q, ++p)
    if (*p != *q)
      return false;
  const char *host = p;
  while (*p && *p != '/' && *p != ':')
    ++p;
  if (p == host)
    return false;  // empty host
  for (const char *h = host; h < p; ++h) {
    char c = *h;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok)
      return false;  // userinfo ('@'), '[', '_' and friends
  }
  if (*p == ':') {
    ++p;
    const char *port = p;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (uint32_t)(*p - '0');
      if (v > 65535)
        return false;
      ++p;
    }
    if (p == port || v == 0)
      return false;
  }
  if (*p == '\0')
    return false;  // a path is required (the offer endpoint)
  return *p == '/';
}
