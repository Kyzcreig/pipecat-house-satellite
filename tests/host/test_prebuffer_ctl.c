/* Host-side unit tests for prebuffer_ctl — gap-resume accounting + Phase 6
 * adaptive prebuffer (NetEQ-lite).
 *
 * Pure C, no ESP-IDF:
 *   ./tests/host/run_prebuffer_tests.sh    (from the repo root)
 * which does: cc -I xiao-esp32-s3/src test_prebuffer_ctl.c prebuffer_ctl.c
 *
 * Covers:
 *  - gap_resumes: full drain + quick refill counted; slow refill (genuine end
 *    of utterance) NOT counted; refill with no prior drain NOT counted;
 *    custom + default resume windows; boundary (== window) counted;
 *    ms-tick wraparound safety.
 *  - adaptive OFF (dark): effective == base ALWAYS (even out of bounds —
 *    runtime /playback/stats overrides must behave exactly as today), zero
 *    transitions no matter how many recoveries stream in.
 *  - adaptive ON: baseline snapshot (pre-existing counter totals ignored),
 *    grow one step when the window rate threshold trips, one step per burst
 *    (window consumed), ceiling clamp at PBC_MAX_MS, decay one step per
 *    quiet period back to base, floor clamp at PBC_MIN_MS, transitions
 *    counts both directions, slow trickle below threshold never grows.
 */
#include <assert.h>
#include <stdio.h>

#include "prebuffer_ctl.h"

static int tests_run = 0;
#define RUN(fn)                 \
  do {                          \
    fn();                       \
    tests_run++;                \
    printf("ok - %s\n", #fn);   \
  } while (0)

/* ── gap_resumes (underrun blind-spot fix) ───────────────────────────────── */

static void test_quick_refill_counts_gap_resume(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0); /* default 750ms window */
  pbc_on_full_drain(&c, 10000);
  assert(pbc_on_refill(&c, 10200) == 1); /* 200ms later = mid-speech gap */
  assert(c.gap_resumes == 1);
}

static void test_slow_refill_is_end_of_utterance(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_full_drain(&c, 10000);
  assert(pbc_on_refill(&c, 13000) == 0); /* 3s later = next utterance */
  assert(c.gap_resumes == 0);
}

static void test_refill_without_drain_not_counted(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  assert(pbc_on_refill(&c, 5000) == 0); /* boot / first utterance */
  assert(c.gap_resumes == 0);
}

static void test_drain_consumed_once(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_full_drain(&c, 1000);
  assert(pbc_on_refill(&c, 1100) == 1);
  assert(pbc_on_refill(&c, 1200) == 0); /* same drain can't double-count */
  assert(c.gap_resumes == 1);
}

static void test_window_boundary_inclusive(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_full_drain(&c, 1000);
  assert(pbc_on_refill(&c, 1750) == 1); /* exactly 750ms = still a gap */
}

static void test_custom_resume_window(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 200); /* PIPECAT_GAP_RESUME_MS=200 */
  pbc_on_full_drain(&c, 1000);
  assert(pbc_on_refill(&c, 1300) == 0); /* 300ms > 200ms window */
  pbc_on_full_drain(&c, 2000);
  assert(pbc_on_refill(&c, 2150) == 1);
  assert(c.gap_resumes == 1);
}

static void test_ms_tick_wraparound(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_full_drain(&c, 0xFFFFFF00u); /* just before uint32 wrap */
  assert(pbc_on_refill(&c, 0x00000064u) == 1); /* 356ms across the wrap */
  assert(c.gap_resumes == 1);
}

static void test_multiple_gaps_accumulate(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  for (uint32_t t = 0; t < 5; t++) {
    pbc_on_full_drain(&c, 10000 + t * 2000);
    assert(pbc_on_refill(&c, 10000 + t * 2000 + 100) == 1);
  }
  assert(c.gap_resumes == 5);
}

/* ── adaptive prebuffer: DARK path (default) ─────────────────────────────── */

static void test_dark_effective_equals_base(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  assert(pbc_effective_ms(&c, 40) == 40);
  assert(pbc_effective_ms(&c, 80) == 80);
  /* Out-of-bounds runtime overrides (/playback/stats?prebuffer_ms=N allows
   * 20..1000) must pass through UNTOUCHED when adaptive is off. */
  assert(pbc_effective_ms(&c, 20) == 20);
  assert(pbc_effective_ms(&c, 1000) == 1000);
}

static void test_dark_never_reacts_to_recoveries(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_track_recoveries(&c, 0, 0); /* baseline */
  for (uint32_t i = 1; i <= 100; i++) {
    pbc_track_recoveries(&c, i * 10, i * 100); /* recovery storm */
  }
  assert(c.offset_steps == 0);
  assert(c.transitions == 0);
  assert(pbc_effective_ms(&c, 80) == 80);
}

/* ── adaptive prebuffer: ENABLED (PIPECAT_ADAPTIVE_PREBUFFER=1) ──────────── */

static void test_adaptive_baseline_snapshot_ignored(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  /* Counter already at 500 from before the controller existed: the first
   * call only snapshots — no instant growth. */
  pbc_track_recoveries(&c, 500, 1000);
  assert(c.offset_steps == 0);
  pbc_track_recoveries(&c, 500, 2000); /* no new events */
  assert(c.offset_steps == 0);
}

static void test_adaptive_grows_on_burst(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  /* PBC_RATE_THRESHOLD (5) recoveries within the window -> +1 step */
  pbc_track_recoveries(&c, 5, 1000);
  assert(c.offset_steps == 1);
  assert(c.transitions == 1);
  assert(pbc_effective_ms(&c, 40) == 80); /* 40 -> 80 */
}

static void test_adaptive_one_step_per_burst(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_recoveries(&c, 7, 1000); /* over threshold: +1 step, window consumed */
  assert(c.offset_steps == 1);
  pbc_track_recoveries(&c, 8, 1100); /* 1 more event: below threshold */
  assert(c.offset_steps == 1);       /* no runaway growth */
}

static void test_adaptive_second_burst_grows_again(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_recoveries(&c, 5, 1000);
  assert(pbc_effective_ms(&c, 40) == 80);
  pbc_track_recoveries(&c, 10, 2000);
  assert(pbc_effective_ms(&c, 40) == 120); /* 80 -> 120 */
  assert(c.transitions == 2);
}

static void test_adaptive_ceiling_clamp(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  for (uint32_t i = 1; i <= 10; i++) {
    pbc_track_recoveries(&c, i * 5, i * 1000); /* burst after burst */
  }
  assert(pbc_effective_ms(&c, 40) == PBC_MAX_MS); /* capped at 160 */
  /* offset_steps itself is bounded (max_steps), not just the clamp */
  assert(c.offset_steps == (PBC_MAX_MS - PBC_MIN_MS) / PBC_STEP_MS);
}

static void test_adaptive_decays_after_quiet(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_recoveries(&c, 5, 1000); /* grow to +1 */
  assert(pbc_effective_ms(&c, 40) == 80);
  /* PBC_DECAY_QUIET_MS (30s) with no recoveries -> back one step */
  pbc_track_recoveries(&c, 5, 1000 + PBC_DECAY_QUIET_MS);
  assert(c.offset_steps == 0);
  assert(pbc_effective_ms(&c, 40) == 40);
  assert(c.transitions == 2); /* up + down both counted */
}

static void test_adaptive_decay_one_step_per_quiet_period(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_recoveries(&c, 5, 1000);
  pbc_track_recoveries(&c, 10, 2000); /* +2 steps: effective 120 @base 40 */
  assert(c.offset_steps == 2);
  uint32_t t = 2000 + PBC_DECAY_QUIET_MS;
  pbc_track_recoveries(&c, 10, t);
  assert(c.offset_steps == 1); /* one step, not straight to base */
  pbc_track_recoveries(&c, 10, t + 1000);
  assert(c.offset_steps == 1); /* quiet timer restarted */
  pbc_track_recoveries(&c, 10, t + PBC_DECAY_QUIET_MS);
  assert(c.offset_steps == 0);
}

static void test_adaptive_never_below_floor(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  /* Decay with zero offset: stays at base, no underflow, no transitions. */
  pbc_track_recoveries(&c, 0, PBC_DECAY_QUIET_MS * 4);
  assert(c.offset_steps == 0);
  assert(c.transitions == 0);
  /* Base below floor gets clamped UP when adaptive is on. */
  assert(pbc_effective_ms(&c, 20) == PBC_MIN_MS);
}

static void test_adaptive_slow_trickle_never_grows(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  /* 1 recovery every 11s: each lands in a fresh window, never >= 5. */
  for (uint32_t i = 1; i <= 20; i++) {
    pbc_track_recoveries(&c, i, i * (PBC_WINDOW_MS + 1000));
  }
  assert(c.offset_steps == 0);
  assert(c.transitions == 0);
}

static void test_adaptive_effective_at_default_base_80(void) {
  /* The new static default is 80ms; one growth step -> 120, two -> 160 (cap). */
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  assert(pbc_effective_ms(&c, 80) == 80);
  pbc_track_recoveries(&c, 5, 1000);
  assert(pbc_effective_ms(&c, 80) == 120);
  pbc_track_recoveries(&c, 10, 2000);
  assert(pbc_effective_ms(&c, 80) == 160);
  pbc_track_recoveries(&c, 15, 3000);
  assert(pbc_effective_ms(&c, 80) == 160); /* clamped */
}

int main(void) {
  RUN(test_quick_refill_counts_gap_resume);
  RUN(test_slow_refill_is_end_of_utterance);
  RUN(test_refill_without_drain_not_counted);
  RUN(test_drain_consumed_once);
  RUN(test_window_boundary_inclusive);
  RUN(test_custom_resume_window);
  RUN(test_ms_tick_wraparound);
  RUN(test_multiple_gaps_accumulate);
  RUN(test_dark_effective_equals_base);
  RUN(test_dark_never_reacts_to_recoveries);
  RUN(test_adaptive_baseline_snapshot_ignored);
  RUN(test_adaptive_grows_on_burst);
  RUN(test_adaptive_one_step_per_burst);
  RUN(test_adaptive_second_burst_grows_again);
  RUN(test_adaptive_ceiling_clamp);
  RUN(test_adaptive_decays_after_quiet);
  RUN(test_adaptive_decay_one_step_per_quiet_period);
  RUN(test_adaptive_never_below_floor);
  RUN(test_adaptive_slow_trickle_never_grows);
  RUN(test_adaptive_effective_at_default_base_80);
  printf("all %d prebuffer_ctl tests passed\n", tests_run);
  return 0;
}
