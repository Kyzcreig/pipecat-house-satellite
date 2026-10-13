#!/usr/bin/env bash
# Hub URL policy (spec v1.3 §4.2 4-state table; t_056748fb): the host test
# row by row, the http.cpp source contract, then a mutation arm per property
# so the test is shown to GATE (a surviving mutant fails the run).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
CXX="${CXX:-c++}"
HDR=xiao-esp32-s3/src/hub_url_policy.h
TEST=tests/host/test_hub_url_policy.cpp
BIN="$(mktemp /tmp/pipecat-hub-url.XXXXXX)"
BK="$(mktemp -d)"
cp "$HDR" "$BK/hub_url_policy.h"
restore() { cp "$BK/hub_url_policy.h" "$HDR"; }
trap 'restore; rm -rf "$BK" "$BIN"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror "$TEST" -o "$BIN" || exit 1
"$BIN" || exit 1
python3 tests/host/test_http_url_source.py || exit 1

fails=0
check() { # name
  if "$CXX" -std=c++17 -Wall -Wextra -Werror "$TEST" -o "$BIN" 2>/dev/null && "$BIN" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (test is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - test goes RED on: $1"
  fi
  restore
}

# 1. fallback branch stripped: 3 URL-class failures never leave nvs.
perl -0pi -e 's/if \(url_failures_ >= kUrlFailureThreshold\)\n\s*state_ = HubUrlState::kBakedFallback;/if (false)\n            state_ = HubUrlState::kBakedFallback;/' "$HDR"
check "fallback branch stripped"
# 2. transport_err counted toward url_failures (the crossover).
perl -0pi -e 's/return e == HubUrlEvent::kResolverNxdomain \|\| e == HubUrlEvent::kHttp4xxUrl;/return e == HubUrlEvent::kResolverNxdomain || e == HubUrlEvent::kHttp4xxUrl || e == HubUrlEvent::kTransportErr;/' "$HDR"
check "transport_err counted (staging restart crosses to production)"
# 3. resolver_timeout counted (AGH blip demotes a good override).
perl -0pi -e 's/return e == HubUrlEvent::kResolverNxdomain \|\| e == HubUrlEvent::kHttp4xxUrl;/return e == HubUrlEvent::kResolverNxdomain || e == HubUrlEvent::kHttp4xxUrl || e == HubUrlEvent::kResolverTimeout;/' "$HDR"
check "resolver_timeout counted (AGH blip)"
# 4. http_403 counted (a staging front door refusing the bench MAC).
perl -0pi -e 's/return e == HubUrlEvent::kResolverNxdomain \|\| e == HubUrlEvent::kHttp4xxUrl;/return e == HubUrlEvent::kResolverNxdomain || e == HubUrlEvent::kHttp4xxUrl || e == HubUrlEvent::kHttp403;/' "$HDR"
check "http_403 counted (staging 403)"
# 5. preflight gate stripped: enter re-probing on preflight_fail too.
perl -0pi -e 's/if \(e == HubUrlEvent::kPreflightOk\) \{\n\s*state_ = HubUrlState::kReprobing;/if (e == HubUrlEvent::kPreflightOk || e == HubUrlEvent::kPreflightFail) {\n          state_ = HubUrlState::kReprobing;/' "$HDR"
check "preflight gate stripped (re-probe on the timer alone)"
# 6. double cutover: a second preflight_ok while re-probing redials again.
perl -0pi -e 's/(case HubUrlState::kReprobing:\n)(\s*if \(e == HubUrlEvent::kConnected\))/$1        if (e == HubUrlEvent::kPreflightOk) return true;\n$2/' "$HDR"
grep -q 'kPreflightOk) return true;' "$HDR" || { echo "mutant 6 did not apply"; exit 1; }
check "second cutover in one interval"
# 7. connected no longer resets the counter.
perl -0pi -e 's/if \(e == HubUrlEvent::kConnected\) \{\n\s*url_failures_ = 0;\n\s*\} else if \(hub_url_event_is_url_class/if (e == HubUrlEvent::kConnected) {\n        } else if (hub_url_event_is_url_class/' "$HDR"
check "CONNECTED stops resetting url_failures"
# 8. https accepted by the validator.
perl -0pi -e 's/const char \*prefix = "http:\/\/";/const char *prefix = "http";/' "$HDR"
check "validator accepts a non-http scheme"

if [ "$fails" -eq 0 ]; then
  echo "all 8 mutations caught; hub_url policy: PASS"
else
  echo "$fails mutation(s) survived"; exit 1
fi
