#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nack_client.h"

static void note_healthy(NackClient* client, uint32_t start_ms,
                         uint32_t start_packets) {
  for (uint32_t i = 0; i < 5; i++) {
    nack_client_note_packet_progress(client, start_ms + i * 20u,
                                     start_packets + i);
  }
}

static void arm_ten(NackClient* client, uint16_t first_seq, uint32_t now_ms) {
  for (uint16_t i = 0; i < 10; i++) {
    assert(nack_client_arm(client, (uint16_t)(first_seq + i), now_ms));
  }
}

int main(void) {
  NackClient client;
  nack_client_init(&client);

  note_healthy(&client, 0, 1000);
  arm_ten(&client, 0, 100);
  nack_client_note_packet_progress(&client, 1100, 1004);
  assert(nack_client_sweep(&client, 1200) == 10);
  int outage_dark = nack_client_should_dark(&client, 1200);
  assert(outage_dark == 0);

  note_healthy(&client, 20000, 2000);
  arm_ten(&client, 100, 20100);
  assert(nack_client_sweep(&client, 20200) == 10);
  int dark_after_valid_failure = nack_client_should_dark(&client, 20200);
  assert(dark_after_valid_failure == 1);
  int dark_before_reprobe = nack_client_should_dark(
      &client, 20200 + NACK_BREAKER_REPROBE_MS - 1);
  assert(dark_before_reprobe == 1);
  int restored_at_sixty_seconds =
      !nack_client_should_dark(&client, 20200 + NACK_BREAKER_REPROBE_MS);
  assert(restored_at_sixty_seconds == 1);

  uint32_t reprobe_ms = 20200 + NACK_BREAKER_REPROBE_MS;
  note_healthy(&client, reprobe_ms + 100, 3000);
  arm_ten(&client, 200, reprobe_ms + 200);
  for (uint16_t seq = 200; seq < 210; seq++) {
    assert(nack_client_take(&client, seq, reprobe_ms + 210) ==
           NACK_TAKE_INWINDOW);
  }
  int healthy_reprobe_armed =
      !nack_client_should_dark(&client, reprobe_ms + 220);
  assert(healthy_reprobe_armed == 1);

  printf("{\"fade_seconds\":1,\"outage_dark\":%d,"
         "\"dark_after_valid_failure\":%d,\"dark_before_reprobe\":%d,"
         "\"restored_at_60s\":%d,\"healthy_reprobe_armed\":%d,"
         "\"gate\":\"PASS\"}\n",
         outage_dark, dark_after_valid_failure, dark_before_reprobe,
         restored_at_sixty_seconds, healthy_reprobe_armed);
  return 0;
}
