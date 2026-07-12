/* nack_client.c — NACK retransmit client. See nack_client.h for the contract.
 *
 * Pure logic, host-testable (tests/host/test_nack_client.c). No ESP-IDF deps.
 */
#include "nack_client.h"

#include <string.h>

void nack_client_init(NackClient* c) {
  if (c == NULL) {
    return;
  }
  memset(c, 0, sizeof(*c));
}

int nack_client_plan_gap(uint16_t first_missing, int gap, uint16_t* out_seqs,
                         int max_out) {
  if (out_seqs == NULL || max_out <= 0) {
    return 0;
  }
  /* RED (N-2) owns <= 2-packet bursts; FEC owns N-1. Only >= 3 is ours. */
  if (gap < NACK_MIN_GAP) {
    return 0;
  }
  int count = gap;
  if (count > NACK_MAX_SEQS) {
    count = NACK_MAX_SEQS; /* resend-amplification cap (spec review blocker 3) */
  }
  if (count > max_out) {
    count = max_out;
  }
  for (int i = 0; i < count; i++) {
    /* Ascending from the oldest missing seq; wraps naturally mod 2^16. */
    out_seqs[i] = (uint16_t)(first_missing + i);
  }
  return count;
}

/* Find the pending slot holding `seq` (active), or -1. */
static int find_slot(const NackClient* c, uint16_t seq) {
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active && c->pending[i].seq == seq) {
      return i;
    }
  }
  return -1;
}

/* First free slot, or the slot with the oldest (smallest) deadline if full. */
static int alloc_slot(const NackClient* c) {
  int oldest = 0;
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (!c->pending[i].active) {
      return i;
    }
    if (c->pending[i].deadline_ms < c->pending[oldest].deadline_ms) {
      oldest = i;
    }
  }
  return oldest; /* table full: reuse the oldest (its rtx never came) */
}

void nack_client_arm(NackClient* c, uint16_t seq, uint32_t now_ms) {
  if (c == NULL) {
    return;
  }
  int slot = find_slot(c, seq);
  if (slot < 0) {
    slot = alloc_slot(c);
  }
  c->pending[slot].seq = seq;
  c->pending[slot].deadline_ms = now_ms + NACK_WAIT_MS;
  c->pending[slot].active = 1;
  c->nack_sent++;
}

NackTakeResult nack_client_take(NackClient* c, uint16_t seq, uint32_t now_ms) {
  if (c == NULL) {
    return NACK_TAKE_UNKNOWN;
  }
  int slot = find_slot(c, seq);
  if (slot < 0) {
    return NACK_TAKE_UNKNOWN;
  }
  c->pending[slot].active = 0; /* consume either way */
  /* Signed compare so wrap of the 32-bit ms clock is handled correctly. */
  if ((int32_t)(now_ms - c->pending[slot].deadline_ms) <= 0) {
    c->nack_recovered++;
    return NACK_TAKE_INWINDOW;
  }
  c->nack_late++;
  return NACK_TAKE_LATE;
}

int nack_client_sweep(NackClient* c, uint32_t now_ms) {
  if (c == NULL) {
    return 0;
  }
  int expired = 0;
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active &&
        (int32_t)(now_ms - c->pending[i].deadline_ms) > 0) {
      c->pending[i].active = 0;
      c->nack_late++;
      expired++;
    }
  }
  return expired;
}
