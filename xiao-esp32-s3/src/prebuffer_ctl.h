/* prebuffer_ctl — playback-ring gap accounting + adaptive prebuffer (NetEQ-lite).
 *
 * Pure C, no ESP-IDF deps: host-testable (tests/host/test_prebuffer_ctl.c),
 * same pattern as components/peer/red_unwrap.{c,h}.
 *
 * Two independent jobs (audio-resilience ladder, SPEC-audio-resilience-ladder.md):
 *
 * 1) GAP-RESUME COUNTING (always on — pure counting, no behavior change).
 *    Blind spot proven 2026-07-11: a FULL ring drain re-arms the prebuffer as
 *    "normal end of utterance" and was NOT counted — only partial-frame strands
 *    incremented `underruns`. Ace heard mid-speech syllable gaps while
 *    underruns=0. Fix: when the ring fully drains and NEW audio arrives within
 *    a resume window (default 750ms, env PIPECAT_GAP_RESUME_MS), that IS a
 *    mid-speech gap -> `gap_resumes` counter. A genuine end of utterance won't
 *    refill within the window (TTS turn gaps are seconds).
 *
 * 2) ADAPTIVE PREBUFFER (Phase 6, NetEQ-lite; env PIPECAT_ADAPTIVE_PREBUFFER=1,
 *    default OFF/dark). Track recovery events (plc+fec+red_recovered deltas);
 *    when the rate in a rolling window exceeds a threshold, grow the effective
 *    prebuffer one step (base -> +40ms per step, bounds 40..160ms). After a
 *    quiet period with no recoveries, decay back one step at a time. When
 *    disabled, pbc_effective_ms() returns base_ms UNTOUCHED (no clamp) — the
 *    dark path is bit-identical to today's static behavior.
 */
#ifndef PREBUFFER_CTL_H
#define PREBUFFER_CTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adaptive tuning defaults (overridable at compile time). */
#ifndef PBC_STEP_MS
#define PBC_STEP_MS 40u /* one growth/decay step */
#endif
#ifndef PBC_MIN_MS
#define PBC_MIN_MS 40u /* effective prebuffer floor */
#endif
#ifndef PBC_MAX_MS
#define PBC_MAX_MS 160u /* effective prebuffer ceiling */
#endif
#ifndef PBC_WINDOW_MS
#define PBC_WINDOW_MS 10000u /* rolling recovery-rate window */
#endif
#ifndef PBC_RATE_THRESHOLD
#define PBC_RATE_THRESHOLD 5u /* recoveries within window -> grow one step */
#endif
#ifndef PBC_DECAY_QUIET_MS
#define PBC_DECAY_QUIET_MS 30000u /* no recoveries for this long -> decay one step */
#endif

typedef struct {
  /* gap-resume detection */
  uint32_t resume_window_ms;
  uint32_t drain_at_ms;
  int drain_pending;
  uint32_t gap_resumes;

  /* adaptive prebuffer */
  int adaptive;             /* 0 = dark: effective == base, no state changes */
  uint32_t offset_steps;    /* current growth above base, in PBC_STEP_MS units */
  uint32_t transitions;     /* cumulative count of step changes (up or down) */
  uint32_t window_start_ms;
  uint32_t window_events;
  uint32_t last_recovery_ms;
  uint32_t last_recovery_total; /* snapshot of the cumulative recovery counter */
  int have_baseline;            /* first pbc_track_recoveries() only snapshots */
} prebuffer_ctl;

/* Initialize. adaptive: enable NetEQ-lite growth/decay. resume_window_ms: the
 * gap-resume window (0 -> default 750). */
void pbc_init(prebuffer_ctl *c, int adaptive, uint32_t resume_window_ms);

/* The playback ring FULLY drained (avail == 0) while playing. Arms the
 * resume window. (Partial-frame strands keep counting `underruns` as before —
 * callers must not double-report the same drain through both paths.) */
void pbc_on_full_drain(prebuffer_ctl *c, uint32_t now_ms);

/* New audio arrived while prebuffering. Returns 1 (and increments
 * gap_resumes) iff a full drain happened within resume_window_ms — i.e. this
 * refill proves the drain was a mid-speech gap, not end of utterance. */
int pbc_on_refill(prebuffer_ctl *c, uint32_t now_ms);

/* Feed the CUMULATIVE recovery counter (plc + fec + red_recovered) and let
 * the adaptive state machine evaluate grow/decay. Safe to call every loop
 * iteration; deltas are computed internally. No-op state-wise when adaptive
 * is off (still tracks the snapshot so a later enable starts clean). */
void pbc_track_recoveries(prebuffer_ctl *c, uint32_t cumulative_recoveries,
                          uint32_t now_ms);

/* Effective prebuffer in ms for the given base. Adaptive off -> base
 * unchanged (exactly today's behavior, including out-of-bounds runtime
 * overrides via /playback/stats?prebuffer_ms=N). Adaptive on ->
 * clamp(base + offset_steps*PBC_STEP_MS, PBC_MIN_MS, PBC_MAX_MS). */
uint32_t pbc_effective_ms(const prebuffer_ctl *c, uint32_t base_ms);

#ifdef __cplusplus
}
#endif

#endif /* PREBUFFER_CTL_H */
