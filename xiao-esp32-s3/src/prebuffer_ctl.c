/* prebuffer_ctl.c — see prebuffer_ctl.h. Pure C, host-testable. */
#include "prebuffer_ctl.h"

#include <string.h>

#define PBC_DEFAULT_RESUME_WINDOW_MS 750u

void pbc_init(prebuffer_ctl *c, int adaptive, uint32_t resume_window_ms) {
  memset(c, 0, sizeof(*c));
  c->adaptive = adaptive ? 1 : 0;
  c->resume_window_ms =
      resume_window_ms ? resume_window_ms : PBC_DEFAULT_RESUME_WINDOW_MS;
}

void pbc_on_full_drain(prebuffer_ctl *c, uint32_t now_ms) {
  c->drain_pending = 1;
  c->drain_at_ms = now_ms;
}

int pbc_on_refill(prebuffer_ctl *c, uint32_t now_ms) {
  if (!c->drain_pending) {
    return 0;
  }
  c->drain_pending = 0;
  /* Unsigned subtraction is wraparound-safe for monotonic ms ticks. */
  if ((uint32_t)(now_ms - c->drain_at_ms) <= c->resume_window_ms) {
    c->gap_resumes++;
    return 1;
  }
  return 0; /* refill came late: genuine end of utterance, not a gap */
}

void pbc_track_recoveries(prebuffer_ctl *c, uint32_t cumulative_recoveries,
                          uint32_t now_ms) {
  if (!c->have_baseline) {
    /* First observation: snapshot only, so pre-existing counter totals from
     * before this controller started never count as fresh recoveries. */
    c->have_baseline = 1;
    c->last_recovery_total = cumulative_recoveries;
    c->window_start_ms = now_ms;
    c->last_recovery_ms = now_ms;
    return;
  }

  uint32_t delta = cumulative_recoveries - c->last_recovery_total;
  c->last_recovery_total = cumulative_recoveries;

  if (!c->adaptive) {
    return; /* dark: observe the snapshot, change nothing */
  }

  if (delta > 0) {
    c->window_events += delta;
    c->last_recovery_ms = now_ms;
  }

  /* Rolling window: reset the event count when the window ages out. */
  if ((uint32_t)(now_ms - c->window_start_ms) >= PBC_WINDOW_MS) {
    c->window_start_ms = now_ms;
    c->window_events = delta > 0 ? delta : 0;
  }

  /* Grow: recovery rate over threshold within the window -> +1 step. */
  if (c->window_events >= PBC_RATE_THRESHOLD) {
    uint32_t max_steps = (PBC_MAX_MS - PBC_MIN_MS) / PBC_STEP_MS;
    if (c->offset_steps < max_steps) {
      c->offset_steps++;
      c->transitions++;
    }
    /* Consume the window so one burst = one step, not a step per call. */
    c->window_events = 0;
    c->window_start_ms = now_ms;
  }

  /* Decay: a full quiet period with no recoveries -> -1 step. */
  if (c->offset_steps > 0 &&
      (uint32_t)(now_ms - c->last_recovery_ms) >= PBC_DECAY_QUIET_MS) {
    c->offset_steps--;
    c->transitions++;
    c->last_recovery_ms = now_ms; /* one step per quiet period */
  }
}

uint32_t pbc_effective_ms(const prebuffer_ctl *c, uint32_t base_ms) {
  if (!c->adaptive) {
    return base_ms; /* dark path: bit-identical to static behavior */
  }
  uint32_t eff = base_ms + c->offset_steps * PBC_STEP_MS;
  if (eff < PBC_MIN_MS) eff = PBC_MIN_MS;
  if (eff > PBC_MAX_MS) eff = PBC_MAX_MS;
  return eff;
}
