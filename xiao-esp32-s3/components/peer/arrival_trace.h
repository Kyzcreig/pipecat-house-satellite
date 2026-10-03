/* arrival_trace.h — downlink arrival/drain event trace (t_5faf78b4).
 *
 * Diagnostic only: compiled in with PIPECAT_ARRIVAL_TRACE=1 at configure time;
 * the default build does not contain it. Records, on the device clock, when
 * each downlink RTP packet is SERVICED by the transport loop (and whether
 * another datagram was already queued behind it), seq gaps, and the playback
 * ring's full-drain / refill edges. Pulled via
 * GET /playback/stats?trace_from=N so drains can be correlated with arrival.
 *
 * Pure C, no ESP-IDF deps (tests/host/test_arrival_trace.c). Multi-writer
 * safe: the slot index is claimed atomically and the record's stamp is
 * written last, so a reader only accepts a record whose stamp == its index.
 */
#ifndef ARRIVAL_TRACE_H
#define ARRIVAL_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AT_ARR = 1,    /* audio RTP serviced; aux bit0 = another datagram queued */
  AT_GAP = 2,    /* seq gap; aux = missing packets (capped 255) */
  AT_LATE = 3,   /* late/dup seq dropped */
  AT_DRAIN = 4,  /* playback ring fully drained; aux = 0 */
  AT_REFILL = 5, /* ring refilled to prebuffer; aux = 1 if gap_resume */
};

typedef struct {
  uint32_t stamp; /* slot index + 1 once written; 0 = empty */
  uint32_t t_ms;  /* device ms (esp_timer) */
  uint16_t seq;   /* RTP seq (AT_ARR/AT_GAP/AT_LATE), else 0 */
  uint8_t kind;
  uint8_t aux;
} at_rec;

/* cap must be a power of two. buf is zeroed. */
void at_init(at_rec *buf, uint32_t cap);
void at_push(uint32_t t_ms, uint16_t seq, uint8_t kind, uint8_t aux);
/* Next index that will be written (records [head-cap, head) may exist). */
uint32_t at_head(void);
/* 1 = *out holds record idx; 0 = not written yet or already overwritten. */
int at_get(uint32_t idx, at_rec *out);

#ifdef __cplusplus
}
#endif

#endif /* ARRIVAL_TRACE_H */
