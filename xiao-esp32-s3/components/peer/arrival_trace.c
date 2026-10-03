/* arrival_trace.c — see arrival_trace.h. Pure C, host-testable. */
#include "arrival_trace.h"

#include <string.h>

static at_rec *s_buf = 0;
static uint32_t s_mask = 0;
static uint32_t s_head = 0;

void at_init(at_rec *buf, uint32_t cap) {
  if (buf == 0 || cap == 0 || (cap & (cap - 1)) != 0) {
    s_buf = 0;
    return;
  }
  memset(buf, 0, cap * sizeof(at_rec));
  s_mask = cap - 1;
  s_head = 0;
  __atomic_store_n(&s_buf, buf, __ATOMIC_RELEASE);
}

void at_push(uint32_t t_ms, uint16_t seq, uint8_t kind, uint8_t aux) {
  at_rec *buf = __atomic_load_n(&s_buf, __ATOMIC_ACQUIRE);
  if (buf == 0) {
    return;
  }
  uint32_t idx = __atomic_fetch_add(&s_head, 1, __ATOMIC_RELAXED);
  at_rec *r = &buf[idx & s_mask];
  __atomic_store_n(&r->stamp, 0, __ATOMIC_RELAXED);
  r->t_ms = t_ms;
  r->seq = seq;
  r->kind = kind;
  r->aux = aux;
  __atomic_store_n(&r->stamp, idx + 1, __ATOMIC_RELEASE);
}

uint32_t at_head(void) { return __atomic_load_n(&s_head, __ATOMIC_ACQUIRE); }

int at_get(uint32_t idx, at_rec *out) {
  at_rec *buf = __atomic_load_n(&s_buf, __ATOMIC_ACQUIRE);
  if (buf == 0 || out == 0) {
    return 0;
  }
  const at_rec *r = &buf[idx & s_mask];
  if (__atomic_load_n(&r->stamp, __ATOMIC_ACQUIRE) != idx + 1) {
    return 0;
  }
  *out = *r;
  /* Re-check: a writer may have reclaimed the slot during the copy. */
  if (__atomic_load_n(&r->stamp, __ATOMIC_ACQUIRE) != idx + 1) {
    return 0;
  }
  return 1;
}
