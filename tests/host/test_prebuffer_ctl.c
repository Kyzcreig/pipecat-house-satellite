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
 *  - arrival stall (U1d): a >PBC_STALL_MS hold that ends while playing grows
 *    one step; end of utterance (no refill) and the next utterance do not;
 *    stall growth decays on the recovery path's quiet timer; dark observes
 *    only. Mutation check: run_prebuffer_stall_mutation.sh.
 */
#include <assert.h>
#include <stdio.h>

#include "prebuffer_ctl.h"

static int tests_run = 0;
#define RUN(fn)               \
  do {                        \
    fn();                     \
    tests_run++;              \
    printf("ok - %s\n", #fn); \
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
  pbc_on_full_drain(&c, 0xFFFFFF00u);          /* just before uint32 wrap */
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
  pbc_track_recoveries(&c, 7,
                       1000); /* over threshold: +1 step, window consumed */
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
  /* The new static default is 80ms; one growth step -> 120, two -> 160 (cap).
   */
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

/* ── arrival-stall input (U1d, t_9afd3ebe) ───────────────────────────────── */

/* Steady 20 ms arrivals from t0 to t1 (inclusive), polled once per packet. */
static uint32_t feed_steady(prebuffer_ctl *c, uint32_t t0, uint32_t t1,
                            int playing) {
  uint32_t t;
  for (t = t0; t <= t1; t += 20) {
    pbc_track_arrival(c, t, playing, t);
  }
  return t - 20;
}

static void test_stall_grows_one_step(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0); /* recovery baseline: plc+fec+red stays 0 */
  uint32_t t = feed_steady(&c, 1000, 2000, 1);
  assert(c.offset_steps == 0 && c.stalls == 0); /* 20 ms cadence: no stall */
  /* 200 ms AP hold (bench .97 Bathroom AP), then audio resumes. */
  assert(pbc_track_arrival(&c, t + 200, 1, t + 200) == 1);
  pbc_track_recoveries(&c, 0, t + 200);
  assert(c.stalls == 1);
  assert(c.offset_steps == 1);
  assert(c.transitions == 1);
  assert(pbc_effective_ms(&c, 120) == 160);
  /* Burst after the hold (drain-all, PR #25): no further growth. */
  feed_steady(&c, t + 201, t + 400, 1);
  assert(c.offset_steps == 1 && c.stalls == 1);
}

static void test_stall_bounds_and_threshold(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_arrival(&c, 1000, 1, 1000);
  /* exactly PBC_STALL_MS is not a stall (strictly greater) */
  assert(pbc_track_arrival(&c, 1000 + PBC_STALL_MS, 1, 1100) == 0);
  /* stall while live, at the resume-window edge: counted */
  assert(pbc_track_arrival(&c, 1100 + 750, 1, 1850) == 1);
  /* stalls beyond the ceiling: offset bounded like the recovery path */
  uint32_t t = 1850;
  for (int i = 0; i < 10; i++) {
    t += 300;
    pbc_track_arrival(&c, t, 1, t);
  }
  assert(c.offset_steps == (PBC_MAX_MS - PBC_MIN_MS) / PBC_STEP_MS);
  assert(pbc_effective_ms(&c, 120) == PBC_MAX_MS);
}

static void test_end_of_utterance_does_not_grow(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  uint32_t t = feed_steady(&c, 1000, 3000, 1);
  /* Utterance ends: no new packet, ring drains, nothing refills. The playback
   * loop keeps polling with the same stamp for seconds. */
  pbc_on_full_drain(&c, t + 140);
  for (uint32_t now = t; now < t + 5000; now += 5) {
    assert(pbc_track_arrival(&c, t, 1, now) == 0);
    pbc_track_recoveries(&c, 0, now);
  }
  /* Next utterance seconds later (beyond the resume window): not a stall. */
  assert(pbc_track_arrival(&c, t + 5000, 1, t + 5000) == 0);
  assert(c.stalls == 0 && c.offset_steps == 0 && c.transitions == 0);
}

static void test_stall_while_not_playing_does_not_grow(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_arrival(&c, 1000, 0, 1000);
  assert(pbc_track_arrival(&c, 1300, 0, 1300) == 0); /* initial prebuffer */
  assert(c.stalls == 0 && c.offset_steps == 0);
}

static void test_first_arrival_only_snapshots(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  /* Stamp 0 at boot, first real packet much later: baseline, not a stall. */
  assert(pbc_track_arrival(&c, 0, 1, 0) == 0);
  assert(pbc_track_arrival(&c, 0, 1, 50) == 0);
  assert(c.stalls == 0);
}

static void test_stall_decays_like_recovery(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_arrival(&c, 1000, 1, 1000);
  pbc_track_arrival(&c, 1250, 1, 1250); /* stall: +1 */
  assert(c.offset_steps == 1);
  /* The stall restarts the quiet timer: no decay just short of it. */
  pbc_track_recoveries(&c, 0, 1250 + PBC_DECAY_QUIET_MS - 1);
  assert(c.offset_steps == 1);
  pbc_track_recoveries(&c, 0, 1250 + PBC_DECAY_QUIET_MS);
  assert(c.offset_steps == 0);
  assert(c.transitions == 2);
  assert(pbc_effective_ms(&c, 120) == 120);
}

static void test_stall_ms_wraparound(void) {
  prebuffer_ctl c;
  pbc_init(&c, 1, 0);
  pbc_track_arrival(&c, 0xFFFFFF00u, 1, 0xFFFFFF00u);
  assert(pbc_track_arrival(&c, 0x00000040u, 1, 0x40u) == 1); /* 320 ms */
  assert(c.offset_steps == 1);
}

static void test_dark_stall_changes_nothing(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_track_recoveries(&c, 0, 0);
  pbc_track_arrival(&c, 1000, 1, 1000);
  uint32_t t = 1000;
  for (int i = 0; i < 50; i++) {
    t += 250;
    pbc_track_arrival(&c, t, 1, t);
    pbc_track_recoveries(&c, 0, t);
  }
  assert(c.stalls == 50); /* observed */
  assert(c.offset_steps == 0);
  assert(c.transitions == 0);
  assert(pbc_effective_ms(&c, 120) == 120);
  assert(pbc_effective_ms(&c, 20) == 20);     /* no clamp when dark */
  assert(pbc_effective_ms(&c, 1000) == 1000); /* runtime override untouched */
}

/* ── I2S DMA lead (t_a57274a4) ───────────────────────────────────────────── */

static void test_dma_lead_zero_before_first_write(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  assert(pbc_dma_lead_ms(&c, 0) == 0);
  assert(pbc_dma_lead_ms(&c, 123456) == 0);
}

static void test_dma_lead_accumulates_and_drains_in_real_time(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_frame_written(&c, 1000, 20);
  pbc_on_frame_written(&c, 1001, 20);
  assert(pbc_dma_lead_ms(&c, 1001) == 39); /* 1040 - 1001 */
  assert(pbc_dma_lead_ms(&c, 1030) == 10);
  assert(pbc_dma_lead_ms(&c, 1040) == 0);
  assert(pbc_dma_lead_ms(&c, 1500) == 0); /* never negative */
}

static void test_dma_lead_reanchors_after_underflow(void) {
  /* A write after the DMA ran dry starts playing NOW, not at the stale end. */
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_frame_written(&c, 1000, 20);
  pbc_on_frame_written(&c, 2000, 20);
  assert(pbc_dma_lead_ms(&c, 2000) == 20);
}

static void test_dma_lead_paced_writer_never_drains(void) {
  /* The playback-task policy: write while lead <= cap. A real-time producer
   * then never sees lead 0 mid-stream, so no false drain/gap_resume. */
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  uint32_t now = 5000;
  int writes = 0;
  for (; now < 6000; now++) {
    if (pbc_dma_lead_ms(&c, now) <= 40) {
      pbc_on_frame_written(&c, now, 20);
      writes++;
    }
    if (now > 5000) {
      assert(pbc_dma_lead_ms(&c, now) > 0);
      assert(pbc_dma_lead_ms(&c, now) <= 60);
    }
  }
  assert(writes == 52); /* 1 s of audio + the 40 ms lead + first frame */
}

static void test_dma_lead_ms_wraparound(void) {
  prebuffer_ctl c;
  pbc_init(&c, 0, 0);
  pbc_on_frame_written(&c, 0xFFFFFFF0u, 20);
  assert(pbc_dma_lead_ms(&c, 0xFFFFFFF0u) == 20);
  assert(pbc_dma_lead_ms(&c, 0x00000002u) == 2);
  assert(pbc_dma_lead_ms(&c, 0x00000004u) == 0);
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
  RUN(test_stall_grows_one_step);
  RUN(test_stall_bounds_and_threshold);
  RUN(test_end_of_utterance_does_not_grow);
  RUN(test_stall_while_not_playing_does_not_grow);
  RUN(test_first_arrival_only_snapshots);
  RUN(test_stall_decays_like_recovery);
  RUN(test_stall_ms_wraparound);
  RUN(test_dark_stall_changes_nothing);
  RUN(test_dma_lead_zero_before_first_write);
  RUN(test_dma_lead_accumulates_and_drains_in_real_time);
  RUN(test_dma_lead_reanchors_after_underflow);
  RUN(test_dma_lead_paced_writer_never_drains);
  RUN(test_dma_lead_ms_wraparound);
  printf("all %d prebuffer_ctl tests passed\n", tests_run);
  return 0;
}
