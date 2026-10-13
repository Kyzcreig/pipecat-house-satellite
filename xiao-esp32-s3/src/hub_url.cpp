// Hub URL runtime: see hub_url.h. Spec v1.3 §4.2 / §7 items 2, 5 (t_056748fb).
#include "hub_url.h"

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_dns.h"
#include "main.h"
#include "nvs.h"

// Policy + the NVS copy are touched from the transport loop (dial, connected),
// the httpd task (POST/DELETE) and the preflight task; every access is under
// this lock and holds it for a few hundred cycles at most.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static HubUrlPolicy s_policy;
static char s_nvs_url[HUB_URL_MAX_LEN + 1] = "";

// Preflight (pass-3 R6): a short-lived task runs the side-effect-free check;
// the transport loop consumes its verdict on its own tick so the policy never
// moves from a foreign task mid-dial.
enum PreflightVerdict : int { kPfIdle = 0, kPfRunning, kPfOk, kPfFail };
static int s_preflight = kPfIdle;
static uint32_t s_preflight_due_ms = 0;
static const char *s_last_dns = "none";

static inline uint32_t now_ms() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void load_nvs_locked() {
  s_nvs_url[0] = '\0';
  nvs_handle_t nvs;
  if (nvs_open(HUB_URL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    return;
  char buf[HUB_URL_MAX_LEN + 1];
  size_t len = sizeof(buf);
  esp_err_t ret = nvs_get_str(nvs, HUB_URL_NVS_KEY, buf, &len);
  nvs_close(nvs);
  if (ret != ESP_OK)
    return;
  if (!hub_url_valid(buf, HUB_URL_MAX_LEN)) {
    // I-2: a malformed value is ignored at read time, never dialed.
    ESP_LOGW(LOG_TAG, "hub/url: malformed NVS value ignored (%u bytes)",
             (unsigned)strlen(buf));
    return;
  }
  strlcpy(s_nvs_url, buf, sizeof(s_nvs_url));
}

void pipecat_hub_url_init() {
  portENTER_CRITICAL(&s_lock);
  load_nvs_locked();
  s_policy.init(s_nvs_url[0] != '\0');
  s_preflight = kPfIdle;
  portEXIT_CRITICAL(&s_lock);
  ESP_LOGI(LOG_TAG, "hub/url: source=%s baked=%s nvs=%s",
           hub_url_state_name(s_policy.state()), PIPECAT_SMALLWEBRTC_URL,
           s_nvs_url[0] ? s_nvs_url : "(unset)");
}

// Split `http://host[:port]/path` (already validated) into parts.
static bool split_url(const char *url, char *host, size_t host_cap,
                      uint16_t *port, const char **path) {
  const char *p = url + strlen("http://");
  const char *h = p;
  while (*p && *p != '/' && *p != ':')
    ++p;
  size_t hl = (size_t)(p - h);
  if (hl == 0 || hl >= host_cap)
    return false;
  memcpy(host, h, hl);
  host[hl] = '\0';
  *port = 80;
  if (*p == ':') {
    ++p;
    unsigned v = 0;
    while (*p >= '0' && *p <= '9')
      v = v * 10 + (unsigned)(*p++ - '0');
    *port = (uint16_t)v;
  }
  *path = p;
  return *p == '/';
}

static bool is_ipv4_literal(const char *host) {
  struct in_addr a;
  return inet_pton(AF_INET, host, &a) == 1;
}

// Map an adapter verdict onto the policy's event ladder (pass-3 B2 table).
static HubUrlEvent dns_event(HubDnsResult r) {
  switch (r) {
    case HubDnsResult::kNxdomain:
      return HubUrlEvent::kResolverNxdomain;
    case HubDnsResult::kServfail:
      return HubUrlEvent::kResolverServfail;
    case HubDnsResult::kNodata:
      return HubUrlEvent::kResolverNodata;
    case HubDnsResult::kTimeout:
    case HubDnsResult::kMismatch:
    case HubDnsResult::kNoResolver:
    default:
      return HubUrlEvent::kResolverTimeout;
  }
}

// Resolve the override host through the designated resolver and rewrite the
// URL with the IP literal. Returns false (event recorded) when the host did
// not yield a usable A answer. An IP-literal host needs no resolver.
static bool resolve_override(const char *url, char *out, size_t cap,
                             HubUrlEvent *failed_event) {
  char host[128];
  uint16_t port;
  const char *path;
  if (!split_url(url, host, sizeof(host), &port, &path)) {
    *failed_event = HubUrlEvent::kResolverTimeout;
    return false;
  }
  if (is_ipv4_literal(host)) {
    strlcpy(out, url, cap);
    s_last_dns = "literal";
    return true;
  }
  uint32_t ip = 0;
  HubDnsDiag diag;
  HubDnsResult r = pipecat_hub_dns_query(host, &ip, &diag);
  s_last_dns = hub_dns_result_name(r);
  if (r != HubDnsResult::kAOk) {
    ESP_LOGW(LOG_TAG,
             "hub/url: override host %s -> %s (tries=%u discarded=%u rcode=%u)",
             host, s_last_dns, diag.tries, diag.discarded, diag.rcode);
    *failed_event = dns_event(r);
    return false;
  }
  struct in_addr a;
  a.s_addr = htonl(ip);
  char ipstr[16];
  inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
  snprintf(out, cap, "http://%s:%u%s", ipstr, (unsigned)port, path);
  return true;
}

bool pipecat_hub_url(char *out, size_t cap) {
  bool dial_nvs;
  char url[HUB_URL_MAX_LEN + 1];
  portENTER_CRITICAL(&s_lock);
  dial_nvs = s_policy.dials_nvs() && s_nvs_url[0] != '\0';
  strlcpy(url, s_nvs_url, sizeof(url));
  portEXIT_CRITICAL(&s_lock);
  if (!dial_nvs) {
    // Baked name: lwIP getaddrinfo inside esp_http_client; its failures are
    // transport-class by construction (the baked states count nothing).
    strlcpy(out, PIPECAT_SMALLWEBRTC_URL, cap);
    return true;
  }
  HubUrlEvent ev;
  if (!resolve_override(url, out, cap, &ev)) {
    pipecat_hub_url_note_result(ev);
    return false;
  }
  return true;
}

void pipecat_hub_url_note_result(HubUrlEvent e) {
  HubUrlState before, after;
  bool redial;
  portENTER_CRITICAL(&s_lock);
  before = s_policy.state();
  redial = s_policy.note(e);
  after = s_policy.state();
  if (after == HubUrlState::kBakedFallback && before != after)
    s_preflight_due_ms = now_ms() + s_policy.preflight_interval_ms();
  portEXIT_CRITICAL(&s_lock);
  if (before != after || hub_url_event_is_url_class(e)) {
    ESP_LOGW(LOG_TAG, "hub/url: %s: %s -> %s (url_failures=%u)",
             hub_url_event_name(e), hub_url_state_name(before),
             hub_url_state_name(after), (unsigned)s_policy.url_failures());
  } else {
    ESP_LOGI(LOG_TAG, "hub/url: %s in %s (url_failures=%u)",
             hub_url_event_name(e), hub_url_state_name(after),
             (unsigned)s_policy.url_failures());
  }
#if PIPECAT_REDIAL
  if (redial)
    pipecat_webrtc_request_redial();
#else
  (void)redial;  // applied on the next re-offer (watchdog restart) or boot
#endif
}

// --- preflight -------------------------------------------------------------

// GET http://ip:port/health within 3 s total; true when the body carries
// `"ok": true`. Opens no offer, touches no PeerConnection.
static bool preflight_health(const char *url) {
  char host[128];
  uint16_t port;
  const char *path;
  if (!split_url(url, host, sizeof(host), &port, &path))
    return false;
  uint32_t ip = 0;
  if (!is_ipv4_literal(host)) {
    HubDnsDiag diag;
    HubDnsResult r = pipecat_hub_dns_query(host, &ip, &diag);
    s_last_dns = hub_dns_result_name(r);
    if (r != HubDnsResult::kAOk)
      return false;
  } else {
    struct in_addr a;
    inet_pton(AF_INET, host, &a);
    ip = ntohl(a.s_addr);
  }
  // TCP connect, 1 s.
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return false;
  struct timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(ip);
  int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  close(fd);
  if (rc != 0)
    return false;
  // GET /health, 2 s.
  struct in_addr a;
  a.s_addr = htonl(ip);
  char ipstr[16];
  inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
  char health_url[64];
  snprintf(health_url, sizeof(health_url), "http://%s:%u/health", ipstr,
           (unsigned)port);
  esp_http_client_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.url = health_url;
  cfg.timeout_ms = 2000;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr)
    return false;
  bool ok = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char body[257];
    int n = esp_http_client_read(client, body, sizeof(body) - 1);
    if (n < 0)
      n = 0;
    body[n] = '\0';
    ok = status == 200 &&
         (strstr(body, "\"ok\":true") || strstr(body, "\"ok\": true"));
  }
  esp_http_client_cleanup(client);
  return ok;
}

static void preflight_task(void *arg) {
  (void)arg;
  char url[HUB_URL_MAX_LEN + 1];
  portENTER_CRITICAL(&s_lock);
  strlcpy(url, s_nvs_url, sizeof(url));
  portEXIT_CRITICAL(&s_lock);
  const bool ok = url[0] != '\0' && preflight_health(url);
  ESP_LOGI(LOG_TAG, "hub/url: preflight %s (dns=%s)", ok ? "ok" : "fail",
           s_last_dns);
  __atomic_store_n(&s_preflight, ok ? kPfOk : kPfFail, __ATOMIC_SEQ_CST);
  vTaskDelete(nullptr);
}

void pipecat_hub_url_tick() {
  const int verdict = __atomic_load_n(&s_preflight, __ATOMIC_SEQ_CST);
  if (verdict == kPfOk || verdict == kPfFail) {
    __atomic_store_n(&s_preflight, kPfIdle, __ATOMIC_SEQ_CST);
    // preflight_ok: the policy enters re-probing and asks for exactly ONE
    // redial (the cutover). preflight_fail: nothing moves.
    pipecat_hub_url_note_result(verdict == kPfOk ? HubUrlEvent::kPreflightOk
                                                 : HubUrlEvent::kPreflightFail);
    portENTER_CRITICAL(&s_lock);
    s_preflight_due_ms = now_ms() + s_policy.preflight_interval_ms();
    portEXIT_CRITICAL(&s_lock);
    return;
  }
  if (verdict != kPfIdle)
    return;
  bool start;
  portENTER_CRITICAL(&s_lock);
  start = s_policy.preflight_armed() &&
          (int32_t)(now_ms() - s_preflight_due_ms) >= 0;
  portEXIT_CRITICAL(&s_lock);
  if (!start)
    return;
  __atomic_store_n(&s_preflight, kPfRunning, __ATOMIC_SEQ_CST);
  if (xTaskCreate(preflight_task, "hub_preflight", 8192, nullptr, 3, nullptr) !=
      pdPASS) {
    __atomic_store_n(&s_preflight, kPfIdle, __ATOMIC_SEQ_CST);
    portENTER_CRITICAL(&s_lock);
    s_preflight_due_ms = now_ms() + s_policy.preflight_interval_ms();
    portEXIT_CRITICAL(&s_lock);
  }
}

// --- operator surface ------------------------------------------------------

int pipecat_hub_url_set(const char *url) {
  if (!hub_url_valid(url, HUB_URL_MAX_LEN))
    return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(HUB_URL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (ret != ESP_OK)
    return ret;
  ret = nvs_set_str(nvs, HUB_URL_NVS_KEY, url);
  if (ret == ESP_OK)
    ret = nvs_commit(nvs);
  nvs_close(nvs);
  if (ret != ESP_OK)
    return ret;
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_nvs_url, url, sizeof(s_nvs_url));
  portEXIT_CRITICAL(&s_lock);
  pipecat_hub_url_note_result(HubUrlEvent::kPost);
  return ESP_OK;
}

int pipecat_hub_url_erase() {
  nvs_handle_t nvs;
  esp_err_t ret = nvs_open(HUB_URL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (ret == ESP_OK) {
    ret = nvs_erase_key(nvs, HUB_URL_NVS_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
      ret = ESP_OK;
    if (ret == ESP_OK)
      ret = nvs_commit(nvs);
    nvs_close(nvs);
  } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ret = ESP_OK;  // namespace never written: already empty
  }
  if (ret != ESP_OK)
    return ret;
  portENTER_CRITICAL(&s_lock);
  s_nvs_url[0] = '\0';
  portEXIT_CRITICAL(&s_lock);
  pipecat_hub_url_note_result(HubUrlEvent::kErase);
  return ESP_OK;
}

const char *pipecat_hub_url_source_name() {
  return hub_url_state_name(s_policy.state());
}
const char *pipecat_hub_url_nvs() {
  return s_nvs_url;
}
const char *pipecat_hub_url_baked() {
  return PIPECAT_SMALLWEBRTC_URL;
}
uint32_t pipecat_hub_url_failures() {
  return s_policy.url_failures();
}
uint32_t pipecat_hub_url_resolver_timeouts() {
  return s_policy.resolver_timeouts();
}
uint32_t pipecat_hub_url_cutovers_failed() {
  return s_policy.cutovers_failed();
}

int pipecat_hub_url_status_json(char *out, size_t cap) {
  char dns[40];
  pipecat_dns_servers_str(dns, sizeof(dns));
  return snprintf(out, cap,
                  "\"hub_url_source\":\"%s\",\"url_failures\":%u,"
                  "\"resolver_timeouts\":%u,\"dns_servers\":\"%s\"",
                  pipecat_hub_url_source_name(),
                  (unsigned)pipecat_hub_url_failures(),
                  (unsigned)pipecat_hub_url_resolver_timeouts(), dns);
}
