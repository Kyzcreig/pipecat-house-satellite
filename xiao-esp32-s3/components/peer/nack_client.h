/* nack_client.h — NACK retransmit client for the audio downlink (Phase 5).
 *
 * VENDORED (2026-07-12, pipecat-house-satellite): the audio-resilience ladder
 * Phase 5 (pipecat-house-voice docs/SPEC-audio-resilience-ladder.md REV 3). On
 * a LAN the RTT is ~1-3ms, so a lost RTP packet can be re-requested and the
 * server's resend still arrives inside the device's 80ms prebuffer — a bit-
 * exact recovery that beats ALL concealment (FEC/RED/PLC synthesize; NACK gets
 * the ORIGINAL opus bytes back).
 *
 * DIVISION OF LABOUR (spec review blocker 4 — latency honesty): FEC covers N-1,
 * RED covers <=2-packet bursts losslessly. NACK's job is ONLY gaps of >= 3
 * packets (and the case where the RED copy of a frame was also lost). rtp.c
 * therefore only calls the planner when delta >= NACK_MIN_GAP (3); <= 2 stays
 * on the RED/FEC/PLC path untouched.
 *
 * PROTOCOL (over the existing RTVI reliable data channel; no new SDP/PT):
 *   device -> server:  {"t":"nack","seqs":[<u16>,...]}   (<= NACK_MAX_SEQS)
 *   server -> device:  {"type":"server-message","data":{"t":"rtx",
 *                        "seq":<u16>,"payload_b64":"<base64 opus>"}}
 * The device splices the rtx opus payload into the decode path if it arrives
 * within NACK_WAIT_MS (20ms, inside the 80ms prebuffer); otherwise it is late
 * and dropped (the RED/PLC fallback already ran for that frame).
 *
 * PURE LOGIC — no ESP-IDF / libpeer deps, so it unit-tests on the host
 * (tests/host/test_nack_client.c). The actual data-channel send + opus splice
 * live in the callers (rtp.c registers a send callback; rtvi.cpp feeds rtx in).
 *
 * ENV GATE: dark unless PIPECAT_NACK=1 (compile-time, add_compile_definitions
 * in the top CMakeLists). With it off, rtp.c never calls the planner and this
 * module is inert.
 */
#ifndef NACK_CLIENT_H_
#define NACK_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only NACK bursts RED can't reach. RED (N-2) owns gaps of <= 2. */
#define NACK_MIN_GAP 3

/* Must match the server's NACK_MAX_SEQS (nack_retransmit.py): one request
 * carries at most this many seqs (the resend-amplification cap). */
#define NACK_MAX_SEQS 8

/* Splice window: an rtx must arrive within this long after the NACK was sent to
 * be usable (inside the 80ms prebuffer). 20ms was too tight for the REAL round
 * trip (measured 2026-07-12: 41/41 rtx arrived but ALL counted late) — the
 * deadline is armed before the JSON even leaves the device, and the reply path
 * crosses the server event loop + SCTP + the RTVI queue task. 60ms fits the
 * measured RTT with margin and still beats the prebuffer. */
#define NACK_WAIT_MS 250

/* Pending-rtx table size: how many in-flight NACKed seqs we track at once. One
 * >=3 burst arms up to NACK_MAX_SEQS; a little headroom for overlapping gaps. */
#define NACK_PENDING_SLOTS 16

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
  uint8_t active;       /* NACK_SLOT_* */
} NackPending;

typedef struct NackClient {
  NackPending pending[NACK_PENDING_SLOTS];
  /* Counters surfaced via /playback/stats (ota.cpp). */
  uint32_t nack_sent;      /* seqs requested (sum over requests) */
  uint32_t nack_recovered; /* rtx spliced in-window (real audio) */
  uint32_t nack_late;      /* rtx arrived after the window (dropped) */
  /* RTT instrumentation: arm->rtx delta of the most recent / worst rtx that
   * REACHED feed_rtx (regardless of in-window/late). Sizes NACK_WAIT_MS from
   * measurement instead of guesswork. */
  uint32_t last_rtt_ms;
  uint32_t max_rtt_ms;
} NackClient;

/* Zero a client (all slots free, counters 0). */
void nack_client_init(NackClient* c);

/* Plan the seqs to NACK for a detected gap of `gap` missing packets whose
 * OLDEST missing seq is `first_missing` (i.e. last_good_seq + 1). Fills
 * out_seqs (ascending, wraps mod 2^16) and returns the count:
 *   - 0 if gap < NACK_MIN_GAP (RED/FEC own it) or bad args,
 *   - min(gap, NACK_MAX_SEQS, max_out) otherwise (the amplification cap).
 * Does NOT arm the pending table — the caller arms after a successful send so a
 * failed send never leaves a phantom pending entry.
 */
int nack_client_plan_gap(uint16_t first_missing, int gap, uint16_t* out_seqs,
                         int max_out);

/* Arm one requested seq as pending (deadline = now_ms + NACK_WAIT_MS) and bump
 * nack_sent. Overwrites the oldest slot if the table is full (a lost rtx just
 * ages out). */
void nack_client_arm(NackClient* c, uint16_t seq, uint32_t now_ms);

/* Offer an arriving rtx for `seq` at `now_ms`. Returns NACK_TAKE_INWINDOW (and
 * bumps nack_recovered + frees the slot) if it was armed and still fresh;
 * NACK_TAKE_LATE (bumps nack_late + frees the slot) if armed but expired;
 * NACK_TAKE_UNKNOWN if we never NACKed it (or already consumed it). */
NackTakeResult nack_client_take(NackClient* c, uint16_t seq, uint32_t now_ms);

/* Sweep expired pending entries at `now_ms`, counting each as late and freeing
 * it. Called opportunistically so an rtx that NEVER arrives still tallies late
 * (otherwise the slot just leaks until overwrite). Returns the number expired. */
int nack_client_sweep(NackClient* c, uint32_t now_ms);

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
