#!/usr/bin/env bash
# Designated-resolver adapter (spec v1.3 §7 items 1a/7, pass-3 B2;
# t_056748fb): hub_dns.cpp against a loopback fake resolver under ASan/UBSan,
# then the three named mutants (any-source accepted, TXID ignored, secondary
# queried on timeout): each must turn the test RED.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
CXX="${CXX:-c++}"
command -v timeout >/dev/null || timeout() { shift; "$@"; }  # macOS without coreutils
SRC=xiao-esp32-s3/src/hub_dns.cpp
TEST=tests/host/test_hub_dns.cpp
BIN="$(mktemp /tmp/pipecat-hub-dns.XXXXXX)"
BK="$(mktemp -d)"
cp "$SRC" "$BK/hub_dns.cpp"
restore() { cp "$BK/hub_dns.cpp" "$SRC"; }
trap 'restore; rm -rf "$BK" "$BIN"' EXIT

build() {
  "$CXX" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I xiao-esp32-s3/src "$TEST" "$SRC" -o "$BIN"
}
build || exit 1
"$BIN" || exit 1

fails=0
check() { # name
  if build 2>/dev/null && timeout 60 "$BIN" >/dev/null 2>&1; then
    echo "MUTATION SURVIVED (test is blind): $1"; fails=$((fails + 1))
  else
    echo "ok - test goes RED on: $1"
  fi
  restore
}

# 1. accept a datagram from ANY source (forged NXDOMAIN becomes NXDOMAIN).
perl -0pi -e 's/if \(from.sin_family != AF_INET \|\| from.sin_port != server.sin_port \|\|\n\s*from.sin_addr.s_addr != server.sin_addr.s_addr\) \{/if (false) {/' "$SRC"
grep -q 'if (false) {' "$SRC" || { echo "mutant 1 did not apply"; exit 1; }
check "any source accepted (forged NXDOMAIN)"
# 2. ignore the TXID.
perl -0pi -e 's/if \(rd16\(buf\) != txid\)\n\s*return HubDnsResult::kMismatch;/(void)txid;/' "$SRC"
grep -q '(void)txid;' "$SRC" || { echo "mutant 2 did not apply"; exit 1; }
check "TXID ignored"
# 3. walk to the next server on timeout (lwIP dns.c shape).
perl -0pi -e 's/(  close\(fd\);\n  return verdict;\n\})/  close(fd);\n  if (verdict == HubDnsResult::kTimeout \&\& server_port != 53) {\n    HubDnsDiag d2;\n    return hub_dns_query_server(name, server_ip, (uint16_t)(server_port + 1), out_ip, \&d2);\n  }\n  return verdict;\n}/' "$SRC"
grep -q 'server_port + 1' "$SRC" || { echo "mutant 3 did not apply"; exit 1; }
check "secondary queried on timeout"

if [ "$fails" -eq 0 ]; then
  echo "all 3 mutations caught; hub_dns adapter: PASS"
else
  echo "$fails mutation(s) survived"; exit 1
fi
