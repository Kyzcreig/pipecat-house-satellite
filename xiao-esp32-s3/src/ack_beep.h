/* ack_beep — satellite-side proof that the hub's wake ACK beep was PLAYED.
 *
 * The hub synthesizes the ack chime and sends it down the WebRTC audio track,
 * so the device cannot tell it apart from other downlink audio by itself. The
 * hub therefore sends an RTVI marker {"t":"ack_beep","id":..,"env":[..],
 * "freqs":[..],"tonal":..} right before the beep PCM. The marker is the
 * REQUEST. This module counts the beep as PLAYED only when audio that
 * i2s_channel_write accepted matches the marker's fingerprint:
 *   - Pearson(per-20ms RMS envelope of played audio, marker env) >= MIN_CORR
 *   - energy-weighted Goertzel tonal fraction at the marker freqs
 *     >= TONAL_FRAC * marker tonal (and the marker itself must be tonal)
 *   - window peak RMS >= MIN_RMS (not matched on near-silence)
 * within TIMEOUT_MS of the marker. Played audio that arrives up to
 * LOOKBACK samples BEFORE the marker is matched too (data channel and RTP are
 * separate paths). A chime played with no marker is never counted.
 *
 * Pure C, no ESP-IDF: host-tested by tests/host/run_ack_beep_tests.sh.
 */
#ifndef PIPECAT_ACK_BEEP_H
#define PIPECAT_ACK_BEEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACK_BEEP_RATE 16000
#define ACK_BEEP_BLOCK 320 /* 20 ms @ 16 kHz */
#define ACK_BEEP_MAX_ENV 16
#define ACK_BEEP_MIN_ENV 4
#define ACK_BEEP_MAX_FREQS 4
#define ACK_BEEP_ID_LEN 32
#define ACK_BEEP_MIN_CORR 0.85f
#define ACK_BEEP_TONAL_FRAC 0.5f
#define ACK_BEEP_MIN_REF_TONAL 0.3f
#define ACK_BEEP_MIN_RMS 64.0f
#define ACK_BEEP_TIMEOUT_MS 3000u
/* Silence padding: when nothing is written for this long while a marker is
 * pending, the device is silent; feed zero blocks so a beep that ends the
 * stream still gets a full window. */
#define ACK_BEEP_IDLE_PAD_MS 40u

typedef struct {
  char id[ACK_BEEP_ID_LEN];
  float env[ACK_BEEP_MAX_ENV];
  int env_len;
  float freqs[ACK_BEEP_MAX_FREQS];
  int nfreqs;
  float tonal;
} ack_beep_marker;

typedef enum {
  ACK_BEEP_ARMED = 0,
  ACK_BEEP_REJECTED = 1, /* malformed / non-tonal marker: never counted */
} ack_beep_arm_result;

typedef struct {
  /* caller-owned history of played samples (ring) */
  int16_t *hist;
  uint32_t hist_cap;
  uint32_t pos;        /* total samples fed (absolute index) */
  uint32_t last_feed_ms;
  uint32_t pad_ms;     /* idle time already converted to zero padding */

  /* pending request */
  int armed;
  ack_beep_marker m;
  uint32_t armed_ms;
  uint32_t scan_from; /* absolute sample index of the next block to score */
  uint32_t consumed;  /* samples at or before this matched an earlier beep */
  float frms[ACK_BEEP_MAX_ENV];
  float fen[ACK_BEEP_MAX_ENV];
  float ftone[ACK_BEEP_MAX_ENV];
  int fcount;

  /* telemetry (monotonic counters + last result) */
  uint32_t marks;
  uint32_t played;
  uint32_t missed;
  uint32_t rejected;
  uint32_t superseded;
  char last_id[ACK_BEEP_ID_LEN];
  uint32_t last_ms;          /* uptime ms of the last PLAYED match */
  int32_t last_corr_milli;   /* corr * 1000 of the last PLAYED match */
  int32_t last_tonal_milli;  /* window tonal * 1000 of the last PLAYED match */
  char last_missed_id[ACK_BEEP_ID_LEN];
  int32_t best_corr_milli;   /* best corr seen for the pending/last marker */
} ack_beep_state;

/* Compact read-only copy for /playback/stats (fits the httpd task stack). */
typedef struct {
  uint32_t marks, played, missed, rejected, superseded;
  int pending;
  char last_id[ACK_BEEP_ID_LEN];
  uint32_t last_ms;
  int32_t last_corr_milli, last_tonal_milli;
  char last_missed_id[ACK_BEEP_ID_LEN];
  int32_t best_corr_milli;
} ack_beep_telemetry;

void ack_beep_get_telemetry(const ack_beep_state *s, ack_beep_telemetry *t);

void ack_beep_init(ack_beep_state *s, int16_t *hist, uint32_t hist_cap);

/* Marker arrived (RTVI task). A still-pending marker is counted missed +
 * superseded. Returns ACK_BEEP_REJECTED for an unusable marker. */
ack_beep_arm_result ack_beep_arm(ack_beep_state *s, const ack_beep_marker *m,
                                 uint32_t now_ms);

/* Samples that i2s_channel_write accepted (playback task). */
void ack_beep_feed(ack_beep_state *s, const int16_t *pcm, uint32_t n,
                   uint32_t now_ms);

/* Periodic tick (playback task idle path): silence padding + timeout. */
void ack_beep_tick(ack_beep_state *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
