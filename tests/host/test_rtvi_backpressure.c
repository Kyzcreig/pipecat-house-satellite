/* Deterministic reproduction of the kitchen NACK-v2 arm reset (t_5c931cb9).
 *
 * Single-threaded virtual-clock model of the firmware failure mechanism — no
 * pthreads, no wall-clock, so it is fully deterministic (a mutation proof must
 * be). It models FreeRTOS xQueueSend semantics on the RTVI receive path:
 *   - block-when-full   == xQueueSend(..., portMAX_DELAY)   (the BUG)
 *   - return-when-full  == xQueueSend(..., 0)               (the FIX)
 *
 * Scenario (faithful to the incident): a sustained inbound 512B RTVI flood
 * arrives on the prio-8 transport loop (pipecat_webrtc_loop -> onmessage ->
 * pipecat_rtvi_handle_message). The prio-2 consumer (rtvi_task) is starved for
 * a window longer than CONFIG_KEEPALIVE_TIMEOUT (30_000ms) — e.g. it loses the
 * core to the loop itself plus the prio-7 audio publisher during the burst — so
 * the 10-deep queue cannot drain.
 *
 * On every loop iteration the transport loop (a) handles one inbound RTVI msg
 * and (b) services one ICE keepalive tick. libpeer declares the peer dead if no
 * keepalive tick lands within the timeout; the real firmware then esp_restart()s
 * (webrtc.cpp:172).
 *
 *   BLOCK: after the 10th enqueue the queue is full; the 11th message blocks the
 *          loop for the ENTIRE starvation window (no slot frees). Keepalive is
 *          not serviced for > timeout -> reset. This is the observed bug.
 *   SHED : each overflow message is dropped in O(0) time; the loop keeps ticking
 *          keepalive every iteration, so no single gap ever exceeds the timeout
 *          -> no reset. This is the fix.
 *
 * Build/run: run_rtvi_backpressure_tests.sh. No ESP-IDF required.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define QUEUE_DEPTH 10
/* Real firmware: CONFIG_KEEPALIVE_TIMEOUT = 30000ms (components/peer). */
#define KEEPALIVE_TIMEOUT_MS 30000u
/* Per-iteration transport-loop cost when it does NOT block (cheap: parse+enqueue
 * or parse+shed + one keepalive tick). Chosen so many iterations fit inside the
 * timeout — the shedding loop must comfortably keep keepalive alive. */
#define LOOP_TICK_MS 5u
/* Consumer (prio-2 rtvi_task) starvation window: it drains nothing for longer
 * than the keepalive timeout. This is what makes a full queue stay full. */
#define STARVE_WINDOW_MS (KEEPALIVE_TIMEOUT_MS + 5000u)
/* Inbound flood spans the whole starvation window at the loop cadence. */
#define FLOOD_MESSAGES (STARVE_WINDOW_MS / LOOP_TICK_MS)

typedef enum { POLICY_BLOCK, POLICY_SHED } send_policy_t;

typedef struct {
  send_policy_t policy;
  bool reset_fired;   // keepalive gap exceeded timeout == firmware esp_restart()
  uint32_t dropped;   // g_rtvi_rx_dropped analogue
  uint32_t delivered; // messages that made it onto the queue
} loop_result_t;

/* One run of the transport loop under a consumer-starved queue. Virtual time in
 * milliseconds; no real sleeping. */
static loop_result_t run_scenario(send_policy_t policy) {
  loop_result_t r = {.policy = policy, .reset_fired = false, .dropped = 0,
                     .delivered = 0};
  uint32_t queue_count = 0;
  uint64_t vnow_ms = 0;
  uint64_t last_keepalive_ms = 0;

  for (uint32_t i = 0; i < FLOOD_MESSAGES; i++) {
    // (a) inbound RTVI handling == pipecat_rtvi_handle_message()
    if (queue_count < QUEUE_DEPTH) {
      queue_count++;  // enqueued
      r.delivered++;
      vnow_ms += LOOP_TICK_MS;
    } else if (policy == POLICY_SHED) {
      r.dropped++;    // xQueueSend(..., 0) on full queue: shed, no wait
      vnow_ms += LOOP_TICK_MS;
    } else {
      // POLICY_BLOCK == portMAX_DELAY on a full queue whose consumer is starved:
      // the loop is stuck here until a slot frees, which does not happen until
      // the end of the starvation window.
      vnow_ms = STARVE_WINDOW_MS;
    }

    // (b) ICE keepalive service. A gap > timeout is the reset.
    if (vnow_ms - last_keepalive_ms > KEEPALIVE_TIMEOUT_MS) {
      r.reset_fired = true;  // firmware would esp_restart() here
      return r;
    }
    last_keepalive_ms = vnow_ms;
  }
  return r;
}

static int tests_run = 0;
#define RUN(fn)                \
  do {                         \
    fn();                      \
    tests_run++;               \
    printf("ok - %s\n", #fn);  \
  } while (0)

/* The BUG reproduces: portMAX_DELAY producer wedges keepalive -> reset. */
static void test_blocking_send_starves_keepalive_and_resets(void) {
  loop_result_t r = run_scenario(POLICY_BLOCK);
  assert(r.reset_fired == true);
  assert(r.dropped == 0);                  // it never sheds; it blocks instead
  assert(r.delivered == QUEUE_DEPTH);      // exactly filled the queue, then died
}

/* The FIX holds: zero-timeout producer sheds overflow, keepalive survives. */
static void test_shedding_send_keeps_keepalive_alive_no_reset(void) {
  loop_result_t r = run_scenario(POLICY_SHED);
  assert(r.reset_fired == false);
  assert(r.dropped > 0);                   // it provably shed under the flood
  assert(r.delivered == QUEUE_DEPTH);      // only the first 10 got queued
  assert(r.delivered + r.dropped == FLOOD_MESSAGES);
}

int main(void) {
  RUN(test_blocking_send_starves_keepalive_and_resets);
  RUN(test_shedding_send_keeps_keepalive_alive_no_reset);
  printf("PASS: %d rtvi backpressure host tests\n", tests_run);
  return 0;
}
