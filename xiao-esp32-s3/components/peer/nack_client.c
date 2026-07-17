/* nack_client.c — NACK retransmit client. See nack_client.h for the contract.
 *
 * Pure logic, host-testable (tests/host/test_nack_client.c). No ESP-IDF deps.
 */
#include "nack_client.h"

#include <string.h>

static uint32_t effect_epoch(uint32_t now_ms) {
  return now_ms / NACK_EFFECT_BUCKET_MS;
}

static NackEffectBucket* effect_bucket(NackClient* c, uint32_t epoch) {
  NackEffectBucket* bucket = &c->effect[epoch % NACK_EFFECT_BUCKETS];
  if (!bucket->valid || bucket->epoch != epoch) {
    memset(bucket, 0, sizeof(*bucket));
    bucket->epoch = epoch;
    bucket->valid = 1;
  }
  return bucket;
}

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

int nack_parse_rtx_frame(const uint8_t* frame, size_t frame_len, uint16_t* seq,
                         const uint8_t** payload, size_t* payload_len) {
  if (frame == NULL || seq == NULL || payload == NULL || payload_len == NULL ||
      frame_len < RTX_FRAME_HEADER_SIZE) {
    return 0;
  }
  uint16_t declared = (uint16_t)(((uint16_t)frame[2] << 8) | frame[3]);
  if (declared == 0 || declared > RTX_FRAME_CAP ||
      frame_len != RTX_FRAME_HEADER_SIZE + (size_t)declared) {
    return 0;
  }
  *seq = (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]);
  *payload = frame + RTX_FRAME_HEADER_SIZE;
  *payload_len = declared;
  return 1;
}

/* Find the pending slot holding `seq` (armed OR expired), or -1. */
static int find_slot(const NackClient* c, uint16_t seq) {
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active != NACK_SLOT_FREE && c->pending[i].seq == seq) {
      return i;
    }
  }
  return -1;
}

/* First free slot, else oldest EXPIRED. Never evict an armed sequence. */
static int alloc_slot(const NackClient* c) {
  int oldest_expired = -1;
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active == NACK_SLOT_FREE) {
      return i;
    }
    if (c->pending[i].active == NACK_SLOT_EXPIRED &&
        (oldest_expired < 0 ||
         c->pending[i].deadline_ms < c->pending[oldest_expired].deadline_ms)) {
      oldest_expired = i;
    }
  }
  return oldest_expired;
}

int nack_client_arm(NackClient* c, uint16_t seq, uint32_t now_ms) {
  if (c == NULL) {
    return 0;
  }
  int slot = find_slot(c, seq);
  if (slot < 0) {
    slot = alloc_slot(c);
  }
  if (slot < 0) {
    return 0;
  }
  c->pending[slot].seq = seq;
  c->pending[slot].deadline_ms = now_ms + NACK_WAIT_MS;
  c->pending[slot].sent_bucket_epoch = effect_epoch(now_ms);
  c->pending[slot].active = NACK_SLOT_ARMED;
  NackEffectBucket* bucket = effect_bucket(c, effect_epoch(now_ms));
  if (bucket->sent < UINT16_MAX) {
    bucket->sent++;
  }
  c->nack_sent++;
  return 1;
}

NackTakeResult nack_client_take(NackClient* c, uint16_t seq, uint32_t now_ms) {
  if (c == NULL) {
    return NACK_TAKE_UNKNOWN;
  }
  int slot = find_slot(c, seq);
  if (slot < 0) {
    return NACK_TAKE_UNKNOWN;
  }
  uint8_t was = c->pending[slot].active;
  c->pending[slot].active = NACK_SLOT_FREE; /* consume either way */
  /* RTT = now - arm time (deadline was arm + NACK_WAIT_MS). Recorded for
   * armed AND expired slots — measuring stragglers is the whole point. */
  {
    uint32_t arm_ms = c->pending[slot].deadline_ms - (uint32_t)NACK_WAIT_MS;
    uint32_t rtt = now_ms - arm_ms; /* wraps correctly in unsigned math */
    c->last_rtt_ms = rtt;
    if (rtt > c->max_rtt_ms) {
      c->max_rtt_ms = rtt;
    }
  }
  /* Signed compare so wrap of the 32-bit ms clock is handled correctly. */
  if (was == NACK_SLOT_ARMED &&
      (int32_t)(now_ms - c->pending[slot].deadline_ms) <= 0) {
    NackEffectBucket* bucket =
        effect_bucket(c, c->pending[slot].sent_bucket_epoch);
    if (bucket->recovered < UINT16_MAX) {
      bucket->recovered++;
    }
    c->nack_recovered++;
    return NACK_TAKE_INWINDOW;
  }
  if (was == NACK_SLOT_ARMED) {
    c->nack_late++; /* expired slots were already counted late at sweep */
  }
  return NACK_TAKE_LATE;
}

int nack_client_sweep(NackClient* c, uint32_t now_ms) {
  if (c == NULL) {
    return 0;
  }
  int expired = 0;
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active == NACK_SLOT_ARMED &&
        (int32_t)(now_ms - c->pending[i].deadline_ms) > 0) {
      c->pending[i].active = NACK_SLOT_EXPIRED; /* keep for RTT; freed on take/reuse */
      c->nack_late++;
      expired++;
    }
  }
  return expired;
}

int nack_client_should_dark(NackClient* c, uint32_t now_ms) {
  if (c == NULL) {
    return 0;
  }
  if (c->auto_dark) {
    return 1;
  }
  for (int i = 0; i < NACK_PENDING_SLOTS; i++) {
    if (c->pending[i].active == NACK_SLOT_ARMED) {
      return 0;
    }
  }

  uint32_t now_epoch = effect_epoch(now_ms);
  uint32_t sent = 0;
  uint32_t recovered = 0;
  for (int i = 0; i < NACK_EFFECT_BUCKETS; i++) {
    NackEffectBucket* bucket = &c->effect[i];
    if (bucket->valid &&
        (uint32_t)(now_epoch - bucket->epoch) < NACK_EFFECT_BUCKETS) {
      sent += bucket->sent;
      recovered += bucket->recovered;
    }
  }
  if (sent >= 10 && recovered * 100u < sent * 20u) {
    c->auto_dark = 1;
  }
  return c->auto_dark;
}
