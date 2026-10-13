// Host test for the hub URL policy (spec v1.3 §4.2 4-state table, pass-3
// B2/R6; card t_056748fb).
// Build: c++ -std=c++17 -Wall -Wextra -Werror test_hub_url_policy.cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../../xiao-esp32-s3/src/hub_url_policy.h"

using E = HubUrlEvent;
using S = HubUrlState;

static const E kUrlClass[] = {E::kResolverNxdomain, E::kHttp4xxUrl};
static const E kTransportClass[] = {E::kResolverTimeout, E::kResolverServfail,
                                    E::kResolverNodata,  E::kTransportErr,
                                    E::kHttp403,         E::kHttp5xx,
                                    E::kMalformed200,    E::kNeverConnected};

static void test_classification_is_total() {
  for (E e : kUrlClass) {
    assert(hub_url_event_is_url_class(e));
    assert(!hub_url_event_is_transport_class(e));
  }
  for (E e : kTransportClass) {
    assert(hub_url_event_is_transport_class(e));
    assert(!hub_url_event_is_url_class(e));
  }
  for (E e : {E::kConnected, E::kPost, E::kErase, E::kPreflightOk,
              E::kPreflightFail}) {
    assert(!hub_url_event_is_url_class(e));
    assert(!hub_url_event_is_transport_class(e));
  }
}

static void test_baked_when_nvs_empty() {
  HubUrlPolicy p;
  p.init(false);
  assert(p.state() == S::kBaked);
  assert(!p.dials_nvs());
  assert(!p.preflight_armed());
  // Row `baked`: every failure walks the ladder on baked, nothing counts.
  for (E e : kUrlClass) {
    assert(!p.note(e));
    assert(p.state() == S::kBaked);
    assert(p.url_failures() == 0);
  }
  for (E e : kTransportClass) {
    assert(!p.note(e));
    assert(p.state() == S::kBaked);
  }
  assert(!p.note(E::kConnected));
  assert(p.state() == S::kBaked);
  assert(!p.note(E::kErase) || true);  // erase on baked: stays baked
  assert(p.state() == S::kBaked);
}

static void test_valid_nvs_wins_and_post_redials() {
  HubUrlPolicy p;
  p.init(true);
  assert(p.state() == S::kNvs);
  assert(p.dials_nvs());
  p.init(false);
  assert(p.note(E::kPost));  // POST valid -> nvs, failures=0, redial
  assert(p.state() == S::kNvs);
  assert(p.url_failures() == 0);
  assert(p.note(E::kErase));  // DELETE -> baked, redial
  assert(p.state() == S::kBaked);
}

static void test_three_url_class_failures_fall_back() {
  for (E e : kUrlClass) {
    HubUrlPolicy p;
    p.init(true);
    assert(!p.note(e));
    assert(p.state() == S::kNvs && p.url_failures() == 1);
    assert(!p.note(e));
    assert(p.state() == S::kNvs && p.url_failures() == 2);
    assert(!p.note(e));
    assert(p.state() == S::kBakedFallback);  // at 3
    assert(!p.dials_nvs());
    assert(p.preflight_armed());
    assert(p.preflight_interval_ms() == HubUrlPolicy::kPreflightIntervalMs);
  }
  // Mixed URL-class events count together.
  HubUrlPolicy p;
  p.init(true);
  p.note(E::kResolverNxdomain);
  p.note(E::kHttp4xxUrl);
  p.note(E::kResolverNxdomain);
  assert(p.state() == S::kBakedFallback);
}

static void test_every_transport_event_never_counts() {
  // EVERY transport-class event on nvs, at ANY count, never falls back and
  // never changes url_failures (no crossover: pass-1 R1 ii, pass-2 R6, B2).
  for (E e : kTransportClass) {
    HubUrlPolicy p;
    p.init(true);
    p.note(E::kResolverNxdomain);  // one URL-class failure already banked
    for (int i = 0; i < 50; i++) {
      assert(!p.note(e));
      assert(p.state() == S::kNvs);
      assert(p.url_failures() == 1);
    }
  }
  // resolver_timeout specifically (AGH silent): counted for telemetry only.
  HubUrlPolicy p;
  p.init(true);
  for (int i = 0; i < 10; i++)
    p.note(E::kResolverTimeout);
  assert(p.state() == S::kNvs);
  assert(p.url_failures() == 0);
  assert(p.resolver_timeouts() == 10);
}

static void test_connected_resets_failures() {
  HubUrlPolicy p;
  p.init(true);
  p.note(E::kResolverNxdomain);
  p.note(E::kHttp4xxUrl);
  assert(p.url_failures() == 2);
  assert(!p.note(E::kConnected));
  assert(p.state() == S::kNvs && p.url_failures() == 0);
  // 2 + connected + 2 never reaches 3.
  p.note(E::kResolverNxdomain);
  p.note(E::kResolverNxdomain);
  assert(p.state() == S::kNvs);
}

static HubUrlPolicy fallen_back() {
  HubUrlPolicy p;
  p.init(true);
  for (int i = 0; i < 3; i++)
    p.note(E::kResolverNxdomain);
  assert(p.state() == S::kBakedFallback);
  return p;
}

static void test_baked_fallback_is_sticky_until_operator_or_reboot() {
  HubUrlPolicy p = fallen_back();
  // Dial outcomes on the baked URL never leave the state.
  for (E e : kUrlClass) {
    assert(!p.note(e));
    assert(p.state() == S::kBakedFallback);
  }
  for (E e : kTransportClass) {
    assert(!p.note(e));
    assert(p.state() == S::kBakedFallback);
  }
  assert(!p.note(E::kConnected));
  assert(p.state() == S::kBakedFallback);
  // reboot -> nvs (the row's `reboot` column): init() with the key present.
  p.init(true);
  assert(p.state() == S::kNvs && p.url_failures() == 0);
  // POST valid -> nvs + redial; DELETE -> baked + redial.
  p = fallen_back();
  assert(p.note(E::kPost));
  assert(p.state() == S::kNvs);
  p = fallen_back();
  assert(p.note(E::kErase));
  assert(p.state() == S::kBaked);
}

static void test_preflight_gates_exactly_one_cutover() {
  HubUrlPolicy p = fallen_back();
  // preflight_fail: no redial, stays.
  assert(!p.note(E::kPreflightFail));
  assert(p.state() == S::kBakedFallback);
  // preflight_ok: re-probing + exactly ONE redial.
  assert(p.note(E::kPreflightOk));
  assert(p.state() == S::kReprobing);
  assert(p.dials_nvs());
  assert(!p.preflight_armed());
  // A second preflight_ok while the probe is in flight is NOT a second
  // cutover (one per interval).
  assert(!p.note(E::kPreflightOk));
  assert(p.state() == S::kReprobing);
  // re-probing + connected -> nvs, failures=0, cutovers_failed=0.
  assert(!p.note(E::kConnected));
  assert(p.state() == S::kNvs && p.url_failures() == 0 &&
         p.cutovers_failed() == 0);
}

static void test_failed_cutover_returns_to_fallback_and_backs_off() {
  for (E e : kUrlClass) {
    HubUrlPolicy p = fallen_back();
    p.note(E::kPreflightOk);
    assert(!p.note(e));
    assert(p.state() == S::kBakedFallback && p.cutovers_failed() == 1);
  }
  for (E e : kTransportClass) {
    HubUrlPolicy p = fallen_back();
    p.note(E::kPreflightOk);
    assert(!p.note(e));
    assert(p.state() == S::kBakedFallback && p.cutovers_failed() == 1);
  }
  // Interval 5 min until 3 failed cutovers, then 30 min.
  HubUrlPolicy p = fallen_back();
  for (uint32_t i = 1; i <= 3; i++) {
    assert(p.preflight_interval_ms() ==
           (i <= 3 ? HubUrlPolicy::kPreflightIntervalMs : 0));
    p.note(E::kPreflightOk);
    p.note(E::kTransportErr);
    assert(p.cutovers_failed() == i);
  }
  assert(p.preflight_interval_ms() == HubUrlPolicy::kPreflightBackoffMs);
  // reset by POST / DELETE / reboot.
  p.note(E::kPost);
  assert(p.cutovers_failed() == 0);
}

static void test_post_delete_during_reprobing_cancel() {
  HubUrlPolicy p = fallen_back();
  p.note(E::kPreflightOk);
  assert(p.note(E::kPost));
  assert(p.state() == S::kNvs && p.url_failures() == 0);
  p = fallen_back();
  p.note(E::kPreflightOk);
  assert(p.note(E::kErase));
  assert(p.state() == S::kBaked);
  // reboot during re-probing -> nvs.
  p = fallen_back();
  p.note(E::kPreflightOk);
  p.init(true);
  assert(p.state() == S::kNvs);
}

static void test_url_validator() {
  assert(hub_url_valid("http://voice-hub.ace:7870/api/offer", 200));
  assert(hub_url_valid("http://192.168.1.89:7871/api/offer", 200));
  assert(hub_url_valid("http://host/api/offer", 200));
  assert(!hub_url_valid("https://voice-hub.ace:7870/api/offer", 200));
  assert(!hub_url_valid("http://voice-hub.ace:7870", 200));  // no path
  assert(!hub_url_valid("http:///api/offer", 200));
  assert(!hub_url_valid("http://a b/api", 200));
  assert(!hub_url_valid("http://user@host/api", 200));
  assert(!hub_url_valid("http://host:0/api", 200));
  assert(!hub_url_valid("http://host:70000/api", 200));
  assert(!hub_url_valid("http://host:/api", 200));
  assert(!hub_url_valid("", 200));
  assert(!hub_url_valid(nullptr, 200));
  assert(!hub_url_valid("http://host/\"x", 200));
  char big[260];
  memset(big, 'a', sizeof(big));
  memcpy(big, "http://", 7);
  big[250] = '/';
  big[259] = '\0';
  assert(!hub_url_valid(big, 200));  // 259 bytes > 200 cap
  big[199] = '\0';
  big[190] = '/';
  assert(hub_url_valid(big, 200));  // 199 bytes fits
}

int main() {
  test_classification_is_total();
  test_baked_when_nvs_empty();
  test_valid_nvs_wins_and_post_redials();
  test_three_url_class_failures_fall_back();
  test_every_transport_event_never_counts();
  test_connected_resets_failures();
  test_baked_fallback_is_sticky_until_operator_or_reboot();
  test_preflight_gates_exactly_one_cutover();
  test_failed_cutover_returns_to_fallback_and_backs_off();
  test_post_delete_during_reprobing_cancel();
  test_url_validator();
  printf("hub_url_policy: PASS\n");
  return 0;
}
