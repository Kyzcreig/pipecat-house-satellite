/* nack_client.h — NACK retransmit client for the audio downlink (Phase 5).
 *
 * VENDORED (2026-07-12, pipecat-house-satellite): the audio-resilience ladder
 * Phase 5 (pipecat-house-voice docs/SPEC-audio-resilience-ladder.md REV 3). On
 * NACK-v2 moves only retransmission replies off the measured-dead ordered RTVI
 * stream. Both channels still share one SCTP association/congestion window;
 * the load+loss B1 gate, not an idle echo, decides whether this stays armed.
 *
 * DIVISION OF LABOUR (spec review blocker 4 — latency honesty): FEC covers N-1,
 * RED covers <=2-packet bursts losslessly. NACK's job is ONLY gaps of >= 3
 * packets (and the case where the RED copy of a frame was also lost). rtp.c
 * therefore only calls the planner when delta >= NACK_MIN_GAP (3); <= 2 stays
 * on the RED/FEC/PLC path untouched.
 *
 * PROTOCOL (request reliable; reply binary unordered + zero-retransmit):
 *   device -> server:  {"t":"nack","seqs":[<u16>,...]}   (<= NACK_MAX_SEQS)
 *   server -> device:  [seq:u16][payload_len:u16][opus bytes]
 * The device splices the rtx opus payload into the decode path if it arrives
 * within NACK_WAIT_MS (60ms, inside the 80ms prebuffer); otherwise it is late
 * and dropped (the RED/PLC fallback already ran for that frame).
 *
 * PURE LOGIC — no ESP-IDF / libpeer deps, so it unit-tests on the host
 * (tests/host/test_nack_client.c). The actual request send + binary frame route
 * live in rtp.c/webrtc.cpp; binary rtx never enters the RTVI JSON parser.
 *
 * ENV GATE: dark unless PIPECAT_NACK=1 (compile-time, add_compile_definitions
 * in the top CMakeLists). With it off, rtp.c never calls the planner and this
 * module is inert.
 */
#ifndef NACK_CLIENT_H_
#define NACK_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#include "nack_protocol_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Only NACK bursts RED can't reach. RED (N-2) owns gaps of <= 2. */
#define NACK_MIN_GAP 3


/* Splice window: an rtx must arrive within this long after the NACK was sent to
 * be usable (inside the 80ms prebuffer). 20ms was too tight for the REAL round
 * trip (measured 2026-07-12: 41/41 rtx arrived but ALL counted late) — the
 * deadline is armed before the JSON even leaves the device, and the reply path
 * crosses the server event loop + SCTP + the RTVI queue task. 60ms fits the
 * measured RTT with margin and still beats the prebuffer. NACK_WAIT_MS is in
 * nack_protocol_generated.h beside the shared wire bounds. */

/* Pending-rtx table size: how many in-flight NACKed seqs we track at once. One
 * >=3 burst arms up to NACK_MAX_SEQS; a little headroom for overlapping gaps. */
#define NACK_PENDING_SLOTS 16
#define NACK_EFFECT_BUCKET_MS 10000u
#define NACK_EFFECT_BUCKETS 60

/* Result of offering an arriving rtx to the pending table. */
typedef enum NackTakeResult {
  NACK_TAKE_UNKNOWN = 0, /* seq was never NACKed (or already consumed) -> ignore */
  NACK_TAKE_INWINDOW = 1, /* armed and still inside NACK_WAIT_MS -> splice it */
  NACK_TAKE_LATE = 2,     /* armed but the window expired -> drop, count late */
} NackTakeResult;

/* Slot states: sweep EXPIRES a slot (counts late once) but keeps it resident
 * so a straggler rtx can still compute its true RTT — the whole point of the
 * RTT counters is sizing NACK_WAIT_MS from measurement, which is impossible
 * if sweeping destroys the arm time before the reply lands. */
#define NACK_SLOT_FREE 0
#define NACK_SLOT_ARMED 1
#define NACK_SLOT_EXPIRED 2

typedef struct NackPending {
  uint16_t seq;
  uint32_t deadline_ms; /* now_ms + NACK_WAIT_MS at arm time */
  uint32_t sent_bucket_epoch;
  uint8_t active;       /* NACK_SLOT_* */
} NackPending;

typedef struct NackEffectBucket {
  uint32_t epoch;
  uint16_t sent;
  uint16_t recovered;
  uint8_t valid;
} NackEffectBucket;

typedef struct NackClient {
  NackPending pending[NACK_PENDING_SLOTS];
  /* Decision counters; rtp.c publishes actual decode-task splice counters. */
  uint32_t nack_sent;      /* seqs requested (sum over requests) */
  uint32_t nack_recovered; /* rtx accepted in-window for bounded staging */
  uint32_t nack_late;      /* rtx late or never arrived before sweep */
  /* RTT instrumentation: arm->rtx delta of the most recent / worst rtx that
   * REACHED feed_rtx (regardless of in-window/late). Sizes NACK_WAIT_MS from
   * measurement instead of guesswork. */
  uint32_t last_rtt_ms;
  uint32_t max_rtt_ms;
  uint8_t auto_dark;
  NackEffectBucket effect[NACK_EFFECT_BUCKETS];
} NackClient;

/* Zero a client (all slots free, counters 0). */
void nack_client_init(NackClient* c);

/* Plan the seqs to NACK for a detected gap of `gap` missing packets whose
 * OLDEST missing seq is `first_missing` (i.e. last_good_seq + 1). Fills
 * out_seqs (ascending, wraps mod 2^16) and returns the count:
 *   - 0 if gap < NACK_MIN_GAP (RED/FEC own it) or bad args,
 *   - min(gap, NACK_MAX_SEQS, max_out) otherwise (the amplification cap).
 * Does NOT arm the pending table; rtp.c arms before send so a failed request
 * still resolves to deferred PLC after NACK_WAIT_MS.
 */
int nack_client_plan_gap(uint16_t first_missing, int gap, uint16_t* out_seqs,
                         int max_out);

/* Parse one exact [seq:u16][payload_len:u16][opus bytes] network-order frame.
 * The returned payload aliases frame. Malformed/empty/oversized frames fail. */
int nack_parse_rtx_frame(const uint8_t* frame, size_t frame_len, uint16_t* seq,
                         const uint8_t** payload, size_t* payload_len);

/* Arm one requested seq as pending (deadline = now_ms + NACK_WAIT_MS) and bump
 * nack_sent. Returns 1 on success. A full table returns 0 without evicting an
 * armed sequence: eviction would suppress that sequence's deferred PLC. */
int nack_client_arm(NackClient* c, uint16_t seq, uint32_t now_ms);

/* Offer an arriving rtx for `seq` at `now_ms`. Returns NACK_TAKE_INWINDOW (and
 * bumps nack_recovered + frees the slot) if it was armed and still fresh;
 * NACK_TAKE_LATE (bumps nack_late + frees the slot) if armed but expired;
 * NACK_TAKE_UNKNOWN if we never NACKed it (or already consumed it). */
NackTakeResult nack_client_take(NackClient* c, uint16_t seq, uint32_t now_ms);

/* Sweep expired pending entries at `now_ms`, counting each as late and retaining
 * its arm time for a straggler RTT sample. Expired slots may be reused by arm().
 * Returns the number newly expired. */
int nack_client_sweep(NackClient* c, uint32_t now_ms);

/* Trip at <20% recovered across >=10 resolved requests in the rolling
 * 10-minute window. Sticky until nack_client_init() on reconnect. */
int nack_client_should_dark(NackClient* c, uint32_t now_ms);

/* ── rtp.c wiring surface (implemented in the vendored rtp.c; declared here so
 * the C++ callers — webrtc.cpp/rtvi.cpp — get the prototypes through one
 * extern-"C" header without touching the pristine submodule rtp.h). ───────── */

/* Data-channel sender: transmits `json` (the NACK request) over the RTVI
 * reliable channel. Registered by webrtc.cpp once the channel is open. */
typedef void (*NackSendFn)(const char* json, size_t len);

/* Register (or clear with NULL) the data-channel sender. Also resets the
 * pending table — a fresh peer means all prior NACKs are moot. */
void rtp_nack_register_sender(NackSendFn fn);

/* Offer an rtx payload (decoded opus bytes for `seq`) arriving over the data
 * channel at `now_ms`. In-window rtx frames are staged for the decode task to
 * splice; returns 1 if accepted, 0 if late/unknown (or NACK compiled out). */
int rtp_nack_feed_rtx(uint16_t seq, const uint8_t* payload, size_t len,
                      uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NACK_CLIENT_H_ */
