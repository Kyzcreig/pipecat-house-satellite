#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../xiao-esp32-s3/src/fleet_identity.h"

static void test_mac_format_is_lowercase_colon() {
  const uint8_t mac[6] = {0x68, 0xEE, 0x8F, 0x46, 0xC2, 0x20};
  char out[PIPECAT_MAC_STR_LEN];
  pipecat_mac_format(mac, out, sizeof(out));
  assert(strcmp(out, "68:ee:8f:46:c2:20") == 0);  // the theater board, as ARP shows it
}

static void test_default_hostname_is_last_three_bytes() {
  const uint8_t mac[6] = {0x68, 0xEE, 0x8F, 0x46, 0xC2, 0x20};
  char out[PIPECAT_MAC_HOSTNAME_LEN];
  pipecat_mac_hostname(mac, out, sizeof(out));
  assert(strcmp(out, "xvf3800-46c220") == 0);  // PRD §5.3 example
}

static void test_request_data_json_shape() {
  char out[256];
  int n = pipecat_request_data_json(
      "dc:b4:d9:38:a6:cc",
      "5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af",
      "1105646", 7, out, sizeof(out));
  assert(n > 0 && (size_t)n < sizeof(out));
  assert(strcmp(out,
                "{\"mac\":\"dc:b4:d9:38:a6:cc\","
                "\"fw_sha256\":\"5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af5d9894af\","
                "\"fw_version\":\"1105646\",\"boot_count\":7}") == 0);
}

static void test_request_data_escapes_and_truncation_is_detectable() {
  char out[256];
  int n = pipecat_request_data_json("00:11:22:33:44:55", "un\"known", "v\\1\n", 0,
                                    out, sizeof(out));
  assert(n > 0 && (size_t)n < sizeof(out));
  assert(strstr(out, "\"fw_sha256\":\"un\\\"known\"") != NULL);
  assert(strstr(out, "\"fw_version\":\"v\\\\1\"") != NULL);  // newline dropped
  char tiny[32];
  n = pipecat_request_data_json("00:11:22:33:44:55", "x", "y", 1, tiny, sizeof(tiny));
  assert((size_t)n >= sizeof(tiny));  // caller must not send a truncated object
  n = pipecat_request_data_json("00:11:22:33:44:55", NULL, NULL, 1, out, sizeof(out));
  assert(strstr(out, "\"fw_sha256\":\"unknown\"") != NULL);
  assert(strstr(out, "\"fw_version\":\"unknown\"") != NULL);
}

int main() {
  test_mac_format_is_lowercase_colon();
  test_default_hostname_is_last_three_bytes();
  test_request_data_json_shape();
  test_request_data_escapes_and_truncation_is_detectable();
  printf("fleet identity: OK\n");
  return 0;
}
