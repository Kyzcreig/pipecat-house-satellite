/* red_unwrap.h — RFC 2198 (RED) depacketizer for the audio downlink.
 *
 * VENDORED (2026-07-11, pipecat-house-satellite): part of the audio-resilience
 * ladder Phase 3 (pipecat-house-voice docs/SPEC-red-rfc2198-burst-redundancy.md
 * + SPEC-audio-resilience-ladder.md REV 3). The server (webrtc_server.py,
 * PIPECAT_RED_ENABLED=1) wraps each opus frame with full copies of the previous
 * 2 frames (N-2). This module parses the RED payload and plans lossless
 * recovery of <=2-packet bursts that FEC (N-1) can't reach.
 *
 * PURE BYTE LOGIC — no ESP-IDF / libpeer dependencies, so it unit-tests on the
 * host (tests/host/test_red_unwrap.c). rtp.c owns the counters and the decoder
 * feed; this file only parses and plans.
 *
 * CRITICAL SAFETY RULE (spec): if RED parsing fails in ANY way the caller MUST
 * fall back to treating the whole payload as a bare opus primary. A RED bug
 * degrades to yesterday's behavior, never breaks the happy path.
 *
 * RFC 2198 payload layout (redundant headers first, primary data last):
 *   redundant block header (4 bytes): |F=1|block PT(7)|ts offset(14)|length(10)|
 *   final/primary header   (1 byte):  |F=0|block PT(7)|
 */
#ifndef RED_UNWRAP_H_
#define RED_UNWRAP_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dynamic PT for RED — must match the server's PIPECAT_RED_PT (default 63)
 * and the SDP offer line in peer_connection.c (a=rtpmap:63 red/48000/2). */
#define RED_PAYLOAD_TYPE 63

/* The server sends N-2; allow headroom for a future N-3 without a reflash. */
#define RED_MAX_BLOCKS 4

/* Cap matches the existing PLC burst cap in rtp.c (>16 = resync, not conceal). */
#define RED_MAX_GAP 16

typedef struct RedBlock {
  uint16_t ts_offset; /* RTP timestamp units BEFORE the primary (14-bit) */
  uint16_t length;    /* block length in bytes (10-bit) */
  const uint8_t* data;
} RedBlock;

typedef struct RedParsed {
  int block_count; /* redundant blocks only (primary excluded) */
  RedBlock blocks[RED_MAX_BLOCKS];
  const uint8_t* primary;
  size_t primary_size;
} RedParsed;

/* Parse an RFC 2198 payload. expected_pt = the opus PT every block must carry
 * (we never mix codecs). Returns 0 on success, -1 on ANY malformation
 * (truncated header, wrong block PT, lengths exceeding the payload, more than
 * RED_MAX_BLOCKS, empty primary). On -1 the caller falls back to primary-only.
 */
int red_unwrap(const uint8_t* payload, size_t size, uint8_t expected_pt,
               RedParsed* out);

/* Recovery plan for a detected seq gap of `gap` missing packets (oldest
 * first). actions[i] is the plan for missing frame i (i=0 -> oldest):
 *   >=0  -> feed parsed->blocks[actions[i]] (lossless RED recovery)
 *   -1   -> no covering block: signal PLC/FEC (NULL feed) as before
 * A block covers missing frame k-packets-before-primary iff its ts_offset ==
 * k * ts_step (ts_step = RTP timestamp units per packet, tracked by rtp.c;
 * 960 for 20ms opus @48k). Returns the action count (== min(gap, RED_MAX_GAP)),
 * or 0 if gap <= 0.
 */
int red_recover_plan(const RedParsed* parsed, int gap, uint32_t ts_step,
                     int8_t actions[RED_MAX_GAP]);

#ifdef __cplusplus
}
#endif

#endif /* RED_UNWRAP_H_ */
