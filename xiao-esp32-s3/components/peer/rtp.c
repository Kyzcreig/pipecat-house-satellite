#include <stdio.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "nack_client.h"
#include "peer_connection.h"
#include "red_unwrap.h"
#include "red_wrap.h"
#include "rtp.h"
#include "utils.h"

typedef enum RtpH264Type {

  NALU = 23,
  FU_A = 28,

} RtpH264Type;

typedef struct NaluHeader {
  uint8_t type : 5;
  uint8_t nri : 2;
  uint8_t f : 1;
} NaluHeader;

typedef struct FuHeader {
  uint8_t type : 5;
  uint8_t r : 1;
  uint8_t e : 1;
  uint8_t s : 1;
} FuHeader;

#define RTP_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader))
#define PT_RED 63
#ifndef PIPECAT_UPLINK_RED
#define PIPECAT_UPLINK_RED 0
#endif
#ifndef PIPECAT_UPLINK_RED_DISTANCE
#define PIPECAT_UPLINK_RED_DISTANCE 4
#endif

#if PIPECAT_UPLINK_RED
static RedWrapState s_uplink_red;
static uint8_t s_uplink_red_payload[RTP_PAYLOAD_SIZE];
#endif
#define FU_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - sizeof(FuHeader) - sizeof(NaluHeader))

int rtp_packet_validate(uint8_t* packet, size_t size) {
  if (size < 12)
    return 0;

  RtpHeader* rtp_header = (RtpHeader*)packet;
  return ((rtp_header->type < 64) || (rtp_header->type >= 96));
}

uint32_t rtp_get_ssrc(uint8_t* packet) {
  RtpHeader* rtp_header = (RtpHeader*)packet;
  return ntohl(rtp_header->ssrc);
}

static int rtp_encoder_encode_h264_single(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);

  // I frame and P frame
  if ((*buf & 0x1f) == 0x05 || (*buf & 0x1f) == 0x01) {
    rtp_packet->header.markerbit = 1;
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }
#if 0
  LOGI("markbit: %d, timestamp: %d, nalu type: %d", rtp_packet->header.markerbit, rtp_encoder->timestamp, buf[0] & 0x1f);
#endif

  memcpy(rtp_packet->payload, buf, size);
  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);
  return 0;
}

static int rtp_encoder_encode_h264_fu_a(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
  uint8_t type = buf[0] & 0x1f;
  uint8_t nri = (buf[0] & 0x60) >> 5;
  buf = buf + 1;
  size = size - 1;

  // increase timestamp if I, P frame
  if (type == 0x05 || type == 0x01) {
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }

  NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
  FuHeader* fu_header = (FuHeader*)rtp_packet->payload + sizeof(NaluHeader);
  fu_header->s = 1;

  while (size > 0) {
    fu_indicator->type = FU_A;
    fu_indicator->nri = nri;
    fu_indicator->f = 0;
    fu_header->type = type;
    fu_header->r = 0;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);

    if (size <= FU_PAYLOAD_SIZE) {
      fu_header->e = 1;
      rtp_packet->header.markerbit = 1;
      memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, size);
      rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader) + sizeof(NaluHeader) + sizeof(FuHeader), rtp_encoder->user_data);
      break;
    }

    fu_header->e = 0;

    memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, FU_PAYLOAD_SIZE);
    rtp_encoder->on_packet(rtp_encoder->buf, CONFIG_MTU, rtp_encoder->user_data);
    size -= FU_PAYLOAD_SIZE;
    buf += FU_PAYLOAD_SIZE;

    fu_header->s = 0;
  }
  return 0;
}

static uint8_t* h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start + 2;

  while (p < buf_end) {
    if (*(p - 2) == 0x00 && *(p - 1) == 0x00 && *p == 0x01)
      return p + 1;
    p++;
  }

  return buf_end;
}

static int rtp_encoder_encode_h264(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  uint8_t* buf_end = buf + size;
  uint8_t *pstart, *pend;
  size_t nalu_size;

  for (pstart = h264_find_nalu(buf, buf_end); pstart < buf_end; pstart = pend) {
    pend = h264_find_nalu(pstart, buf_end);
    nalu_size = pend - pstart;

    if (pend != buf_end)
      nalu_size--;

    while (pstart[nalu_size - 1] == 0x00)
      nalu_size--;

    if (nalu_size <= RTP_PAYLOAD_SIZE) {
      rtp_encoder_encode_h264_single(rtp_encoder, pstart, nalu_size);

    } else {
      rtp_encoder_encode_h264_fu_a(rtp_encoder, pstart, nalu_size);
    }
  }

  return 0;
}

static int rtp_encoder_encode_generic(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpHeader* rtp_header = (RtpHeader*)rtp_encoder->buf;
  rtp_header->version = 2;
  rtp_header->padding = 0;
  rtp_header->extension = 0;
  rtp_header->csrccount = 0;
  rtp_header->markerbit = 0;
  uint8_t payload_type = rtp_encoder->type;
  uint8_t* payload = buf;
  size_t payload_size = size;
#if PIPECAT_UPLINK_RED
  if (rtp_encoder->type == PT_OPUS) {
    int wrapped = red_wrap_packet(&s_uplink_red, buf, size,
                                  rtp_encoder->timestamp,
                                  s_uplink_red_payload,
                                  sizeof(s_uplink_red_payload));
    if (wrapped >= 0) {
      payload_type = PT_RED;
      payload = s_uplink_red_payload;
      payload_size = (size_t)wrapped;
    }
  }
#endif
  rtp_header->type = payload_type;
  rtp_header->seq_number = htons(rtp_encoder->seq_number++);
  rtp_header->timestamp = htonl(rtp_encoder->timestamp);
  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  rtp_header->ssrc = htonl(rtp_encoder->ssrc);
  memcpy(rtp_encoder->buf + sizeof(RtpHeader), payload, payload_size);

  rtp_encoder->on_packet(rtp_encoder->buf, payload_size + sizeof(RtpHeader), rtp_encoder->user_data);

  return 0;
}

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_encoder->on_packet = on_packet;
  rtp_encoder->user_data = user_data;
  rtp_encoder->timestamp = 0;
  rtp_encoder->seq_number = 0;

  switch (codec) {
    case CODEC_H264:
      rtp_encoder->type = PT_H264;
      rtp_encoder->ssrc = SSRC_H264;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_h264;
      break;
    case CODEC_PCMA:
      rtp_encoder->type = PT_PCMA;
      rtp_encoder->ssrc = SSRC_PCMA;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_PCMU:
      rtp_encoder->type = PT_PCMU;
      rtp_encoder->ssrc = SSRC_PCMU;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_OPUS:
      rtp_encoder->type = PT_OPUS;
      rtp_encoder->ssrc = SSRC_OPUS;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
#if PIPECAT_UPLINK_RED
      red_wrap_init(&s_uplink_red, PT_OPUS, PIPECAT_UPLINK_RED_DISTANCE);
#endif
      break;
    default:
      break;
  }
}

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* buf, size_t size) {
  return rtp_encoder->encode_func(rtp_encoder, (uint8_t*)buf, size);
}

static int rtp_decode_h264(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  static const uint32_t nalu_start_4bytecode = 0x01000000;
  static uint8_t nalu_buf[CONFIG_MAX_NALU_SIZE];
  static int offset = 0;
  RtpPacket* rtp_packet = (RtpPacket*)buf;
  uint8_t nalu_type = *rtp_packet->payload & 0x1f;
  int payload_size = size - sizeof(RtpHeader);
  if (nalu_type > 0 && nalu_type < 24) {
    // NALU type 1-23 are single NALUs
    memcpy(nalu_buf, &nalu_start_4bytecode, sizeof(nalu_start_4bytecode));
    offset = sizeof(nalu_start_4bytecode);
    memcpy(nalu_buf + offset, rtp_packet->payload, payload_size);
    offset += payload_size;
    if (rtp_decoder->on_packet != NULL) {
      rtp_decoder->on_packet(nalu_buf, offset, rtp_decoder->user_data);
    }
    return (int)size;
  } else {
    NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
    FuHeader* fu_header = (FuHeader*)(rtp_packet->payload + sizeof(NaluHeader));
    uint8_t reconstructed_nalu_type = (fu_indicator->f << 7) |
                                      (fu_indicator->nri << 5) |
                                      fu_header->type;
    payload_size -= sizeof(NaluHeader) + sizeof(FuHeader);
    if (fu_header->s) {
      memcpy(nalu_buf, &nalu_start_4bytecode, sizeof(nalu_start_4bytecode));
      offset = sizeof(nalu_start_4bytecode);
      memcpy(nalu_buf + offset, &reconstructed_nalu_type, 1);
      offset += 1;
      memcpy(nalu_buf + offset, rtp_packet->payload + 2, payload_size);
      offset += payload_size;
    } else if (offset < CONFIG_MAX_NALU_SIZE) {
      memcpy(nalu_buf + offset, rtp_packet->payload + 2, payload_size);
      offset += payload_size;
      if (fu_header->e) {
        // end of fragmented NALU
        if (rtp_decoder->on_packet != NULL) {
          rtp_decoder->on_packet(nalu_buf, offset, rtp_decoder->user_data);
        }
        offset = 0;  // reset for next NALU
      }
    }
  }
  return 0;
}

// Reorder-vs-loss counters (2026-07-09): exposed via /playback/stats.
volatile uint32_t g_rtp_late_drops = 0;   // packets arriving behind playback (reordering)
volatile uint32_t g_rtp_gap_events = 0;   // distinct seq gaps (PLC bursts fired)

// RED / RFC 2198 counters (2026-07-11, audio-resilience ladder Phase 3).
// packets_received is the frozen loss_burden denominator (ladder spec REV 3):
//   loss_burden = (plc + fec + red_recovered + nack_recovered) / packets_received
volatile uint32_t g_rtp_packets_received = 0;  // ALL audio RTP packets in
volatile uint32_t g_red_recovered = 0;         // lost frames recovered from RED blocks
volatile uint32_t g_red_dup_drops = 0;         // late/dup RED packets dropped (redundancy discarded)

// --- NACK retransmit client (2026-07-12, audio-resilience ladder Phase 5) -----
// On a seq gap of >= NACK_MIN_GAP (3) packets — deeper than RED's N-2 reach —
// re-request the missing seqs over the RTVI data channel; a resend that lands
// within NACK_WAIT_MS (20ms, inside the 80ms prebuffer) is spliced as REAL
// audio. Bit-exact recovery that beats concealment. DARK unless PIPECAT_NACK is
// compiled in (top CMakeLists add_compile_definitions). Pure decision + table
// logic lives in nack_client.{c,h} (host-tested); this file owns the wiring:
// building the request JSON, calling the registered data-channel sender, and
// feeding an arriving rtx back into the live decoder.
//
// DEFERRED CONCEALMENT (the ordering problem): the pre-NACK code conceals a
// gap SYNCHRONOUSLY (RED block or (NULL,0) PLC signal per missing frame,
// inline in rtp_decode_generic). If we both concealed immediately AND spliced
// an rtx ~3ms later, the ring would get TWO frames for the same timestamp
// (PLC + real = audible time stretch). Fix: for seqs we NACK we emit NOTHING
// at gap time — the seq is only "pending" in g_nack's table. Resolution is
// exclusive by construction of nack_client:
//   * rtx arrives in-window  -> nack_client_take() consumes the slot
//     (recovered); a later sweep can no longer see it -> no PLC. Real audio.
//   * rtx never arrives      -> on the NEXT packet arrival (~20ms, matching
//     NACK_WAIT_MS, still inside the 80ms prebuffer) nack_client_sweep()
//     consumes the expired slot (late) and we emit the deferred (NULL,0) PLC
//     signal THEN; a later rtx take() returns UNKNOWN -> not spliced.
// Exactly one of {rtx splice, deferred PLC} ever fires per NACKed seq.
//
// THREADING: rtp_decode_generic runs on the peer-connection task while
// rtp_nack_feed_rtx is called from the RTVI message task. The opus decoder
// behind s_audio_on_packet is NOT thread-safe, so feed_rtx never calls it —
// an in-window rtx is validated (take) and parked in a tiny staging FIFO;
// the decode task drains the FIFO into the decoder at the next packet
// arrival (same instant the deferred-PLC sweep runs). g_nack + the FIFO are
// the only shared state and are guarded by one portMUX with tiny critical
// sections (table scans + <=512B memcpy; the heavy opus work stays outside).
//
// Counters exposed via /playback/stats (ota.cpp):
//   nack_sent      seqs requested
//   nack_recovered rtx spliced in-window (real audio)
//   nack_late      rtx arrived after the 20ms window / never arrived (PLC ran)
static NackClient g_nack;
volatile uint32_t g_nack_sent = 0;
volatile uint32_t g_nack_recovered = 0;
volatile uint32_t g_nack_late = 0;
volatile uint32_t g_nack_last_rtt_ms = 0;
volatile uint32_t g_nack_max_rtt_ms = 0;
// Raw arrivals at feed_rtx BEFORE take() — splits "rtx never delivered"
// (arrived=0) from "delivered but slot already swept/late" (arrived>0,
// recovered=0). The UNKNOWN take path is otherwise invisible.
volatile uint32_t g_nack_rtx_arrived = 0;

// Data-channel sender, registered by the C++ side (webrtc.cpp). NULL until a
// peer is up; a NULL sender means we simply don't NACK (degrade to RED/PLC).
static NackSendFn s_nack_send = NULL;

// The live audio decoder's callback, captured in rtp_decoder_init so an rtx
// arriving OUTSIDE rtp_decode_generic (over the data channel, later) can be
// injected into the same decode path a normal packet takes. One audio
// decoder -> one slot.
static RtpOnPacket s_audio_on_packet = NULL;
static void* s_audio_user_data = NULL;

#ifdef PIPECAT_NACK
#include "freertos/FreeRTOS.h"

#include "esp_timer.h"

static portMUX_TYPE s_nack_mux = portMUX_INITIALIZER_UNLOCKED;

// Staging FIFO: in-window rtx payloads parked by the RTVI task until the
// decode task splices them (opus frames are <400B; server rtx cap matches).
// Sized to NACK_MAX_SEQS so one full 8-seq burst can land between packets.
// RED-wrapped rtx = primary + 2 redundant blocks + headers (~3x bare opus).
// MUST match the rtvi.cpp decode buffer (1024) — 512 silently rejected every
// RED-wrapped rtx BEFORE take(), which is why RTT stayed 0 while arrivals
// counted (the 2026-07-12 trace).
#define NACK_RTX_MAX_PAYLOAD 1024
static struct {
  uint16_t len;
  uint8_t data[NACK_RTX_MAX_PAYLOAD];
} s_rtx_q[NACK_MAX_SEQS];
static volatile uint8_t s_rtx_head = 0;  // producer: rtvi task
static volatile uint8_t s_rtx_tail = 0;  // consumer: decode task

static inline uint32_t nack_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// Decode-task side of deferred concealment, run once per arriving packet
// BEFORE the packet's own gap handling: (1) splice any staged rtx frames
// (real recovered audio) into the decoder, (2) emit the deferred (NULL,0)
// PLC signal for every NACKed seq whose 20ms window expired without an rtx.
static void nack_sweep_and_drain(uint32_t now_ms) {
  for (;;) {
    uint8_t buf[NACK_RTX_MAX_PAYLOAD];
    uint16_t len = 0;
    portENTER_CRITICAL(&s_nack_mux);
    if (s_rtx_tail != s_rtx_head) {
      len = s_rtx_q[s_rtx_tail].len;
      memcpy(buf, s_rtx_q[s_rtx_tail].data, len);
      s_rtx_tail = (uint8_t)((s_rtx_tail + 1) % NACK_MAX_SEQS);
    }
    portEXIT_CRITICAL(&s_nack_mux);
    if (len == 0) {
      break;
    }
    if (s_audio_on_packet != NULL) {
      // The ring stores EXACT wire bytes — RED-wrapped when RED is armed.
      // The normal receive path unwraps RED before on_packet; do the same
      // here or the opus decoder eats RED headers as audio. Safety rule as
      // everywhere: any parse failure -> treat as bare opus.
      RedParsed red;
      if (red_unwrap(buf, len, PT_OPUS, &red) == 0) {
        s_audio_on_packet((uint8_t*)red.primary, red.primary_size,
                          s_audio_user_data);
      } else {
        s_audio_on_packet(buf, len, s_audio_user_data);
      }
    }
  }
  portENTER_CRITICAL(&s_nack_mux);
  int expired = nack_client_sweep(&g_nack, now_ms);
  g_nack_late = g_nack.nack_late;
  portEXIT_CRITICAL(&s_nack_mux);
  for (int i = 0; i < expired; i++) {
    if (s_audio_on_packet != NULL) {
      s_audio_on_packet(NULL, 0, s_audio_user_data);
    }
  }
}

// Build "{\"t\":\"nack\",\"seqs\":[s0,s1,...]}" for `count` seqs and send it over
// the data channel. Arms each seq BEFORE sending: an armed entry is what
// drives the deferred PLC (sweep), so even a lost/failed request degrades to
// correct concealment ~20ms later — and the rtx can never race an unarmed
// table (LAN RTT is ~1-3ms).
static void nack_request(const uint16_t* seqs, int count, uint32_t now_ms) {
  if (s_nack_send == NULL || count <= 0) {
    return;
  }
  portENTER_CRITICAL(&s_nack_mux);
  for (int i = 0; i < count; i++) {
    nack_client_arm(&g_nack, seqs[i], now_ms);
  }
  g_nack_sent = g_nack.nack_sent;
  portEXIT_CRITICAL(&s_nack_mux);
  // Max envelope: {"type":"nack","t":"nack","seqs":[]} = 35 chars + 8 * 6.
  // The "type" key exists ONLY because pipecat's SmallWebRTC on_message does
  // json_message["type"] (KeyError -> message dropped before app-message
  // handlers run); the server's parse_nack_message keys on "t" == "nack".
  char buf[128];
  int n = 0;
  n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                "{\"type\":\"nack\",\"t\":\"nack\",\"seqs\":[");
  for (int i = 0; i < count && n < (int)sizeof(buf) - 8; i++) {
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%u", i ? "," : "",
                  (unsigned)seqs[i]);
  }
  n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");
  s_nack_send(buf, (size_t)n);
}
#endif  // PIPECAT_NACK

void rtp_nack_register_sender(NackSendFn fn) {
  s_nack_send = fn;
  nack_client_init(&g_nack);
}

// Fed by rtvi.cpp when a {"t":"rtx","seq":..,"payload_b64":..} message
// arrives (RTVI task). Returns 1 if the rtx was accepted for splicing
// (in-window; staged for the decode task), 0 otherwise (late/unknown/full).
int rtp_nack_feed_rtx(uint16_t seq, const uint8_t* payload, size_t len,
                      uint32_t now_ms) {
  g_nack_rtx_arrived++;
#ifdef PIPECAT_NACK
  if (payload == NULL || len == 0 || len > NACK_RTX_MAX_PAYLOAD) {
    return 0;
  }
  int staged = 0;
  portENTER_CRITICAL(&s_nack_mux);
  NackTakeResult r = nack_client_take(&g_nack, seq, now_ms);
  if (r == NACK_TAKE_INWINDOW) {
    uint8_t next = (uint8_t)((s_rtx_head + 1) % NACK_MAX_SEQS);
    if (next != s_rtx_tail) {  // FIFO not full
      s_rtx_q[s_rtx_head].len = (uint16_t)len;
      memcpy(s_rtx_q[s_rtx_head].data, payload, len);
      s_rtx_head = next;
      staged = 1;
    }
    // FIFO full (can't happen with slots == NACK_MAX_SEQS unless the server
    // misbehaves): the frame is dropped; take() already consumed the slot so
    // the sweep won't double-conceal — the frame is simply lost, as pre-NACK.
  }
  g_nack_recovered = g_nack.nack_recovered;
  g_nack_last_rtt_ms = g_nack.last_rtt_ms;
  g_nack_max_rtt_ms = g_nack.max_rtt_ms;
  g_nack_late = g_nack.nack_late;
  portEXIT_CRITICAL(&s_nack_mux);
  return staged;
#else
  (void)seq;
  (void)payload;
  (void)len;
  (void)now_ms;
  return 0;
#endif
}


static int rtp_decode_generic(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)buf;
  if (size < sizeof(RtpHeader)) {
    return -1;
  }
  g_rtp_packets_received++;

#ifdef PIPECAT_NACK
  // Deferred-concealment resolution point (see the NACK block above): on
  // EVERY audio packet arrival — ~20ms after the previous one, matching
  // NACK_WAIT_MS — first splice any staged rtx frames (real recovered audio),
  // then emit the deferred PLC signal for NACKed seqs whose window expired.
  // Runs BEFORE this packet's own gap handling and payload delivery.
  if (rtp_decoder->on_packet != NULL &&
      rtp_decoder->on_packet == s_audio_on_packet) {
    nack_sweep_and_drain(nack_now_ms());
  }
#endif

  // --- VENDORED PATCH (2026-07-11): RED / RFC 2198 unwrap -------------------
  // The server (webrtc_server.py, PIPECAT_RED_ENABLED=1) wraps opus in RED:
  // every packet carries full copies of the previous 2 frames (N-2), so any
  // <=2-packet burst is recovered LOSSLESSLY (FEC only reaches N-1; deeper
  // bursts previously fell to synthetic PLC). Parse the RED chain here; the
  // primary block rides the normal decode path, redundant blocks fill seq
  // gaps below. SAFETY RULE (spec): if parsing fails in ANY way, treat the
  // whole payload as bare opus — RED bugs degrade to yesterday's behavior.
  uint8_t* payload = rtp_packet->payload;
  size_t payload_size = size - sizeof(RtpHeader);
  RedParsed red;
  int red_ok = 0;
  if (rtp_packet->header.type == RED_PAYLOAD_TYPE) {
    if (red_unwrap(payload, payload_size, PT_OPUS, &red) == 0) {
      red_ok = 1;
      payload = (uint8_t*)red.primary;
      payload_size = red.primary_size;
    }
    // else: fail-safe fallthrough — payload stays the full buffer (bare opus)
  }
  // --- END RED unwrap --------------------------------------------------------

  // --- VENDORED PATCH (2026-07-09, pipecat-house-satellite) ---
  // Upstream ignores RTP sequence numbers entirely: no loss detection, no
  // reorder handling, no PLC signal. Opus is a predictive codec — feeding it
  // a stream with silent gaps produces an audible click/crackle at every
  // lost/reordered UDP packet (root-caused via flash-selftest A/B: same
  // bytes clean without opus/network, crackly with; delivery counters clean).
  // Fix: track seq per decoder; on a gap, signal the consumer BEFORE the new
  // payload with (NULL, 0) callbacks so it can run opus PLC
  // (opus_decode(dec, NULL, ...)) once per missing frame — unless a RED
  // redundant block covers the missing frame, in which case feed REAL audio.
  // Late/duplicate packets (seq behind) are dropped — the ring already played
  // past them. Seq state lives in a tiny side-table keyed by decoder pointer
  // (can't add fields to RtpDecoder — rtp.h stays in the pristine submodule).
  enum { RTP_SEQ_SLOTS = 4 };
  static struct {
    RtpDecoder* dec;
    uint16_t last_seq;
    uint32_t last_ts;
    uint8_t initialized;
  } seq_state[RTP_SEQ_SLOTS];
  int slot = -1;
  for (int i = 0; i < RTP_SEQ_SLOTS; i++) {
    if (seq_state[i].dec == rtp_decoder) { slot = i; break; }
    if (slot < 0 && seq_state[i].dec == NULL) slot = i;
  }
  if (slot >= 0) {
    uint16_t seq = ntohs(rtp_packet->header.seq_number);
    uint32_t ts = ntohl(rtp_packet->header.timestamp);
    if (seq_state[slot].dec == rtp_decoder && seq_state[slot].initialized) {
      uint16_t expected = (uint16_t)(seq_state[slot].last_seq + 1);
      int16_t delta = (int16_t)(seq - expected);
      if (delta < 0) {
        // Late or duplicate packet — playback has moved on; drop it.
        // COUNTED (2026-07-09): late-drops vs gap-events splits REORDERING
        // from true LOSS. High plc + high late = reordering (fix = small
        // reorder buffer here, no WiFi change); high plc + late≈0 = real
        // loss (fix = WiFi/AP). Query via GET /playback/stats.
        // RED (2026-07-11): NEVER feed a late packet's redundant blocks —
        // their successors already decoded (decoder-state corruption
        // otherwise, spec §Decoder-feed-order). Count the discard.
        g_rtp_late_drops++;
        if (red_ok) {
          g_red_dup_drops++;
        }
        return (int)size;
      }
      if (delta > 0 && delta <= 16 && rtp_decoder->on_packet != NULL) {
        // Gap: delta packets lost. Cap the recovery burst (>16 ≈ >320ms means
        // a real outage — resync instead of interpolating a long stretch).
        // RED first: a redundant block whose ts_offset matches a missing
        // frame carries the REAL encoded audio — feed it in timestamp order
        // (oldest first) so the opus decoder state stays coherent. Frames
        // without a covering block keep the (NULL, 0) PLC/FEC signal.
        g_rtp_gap_events++;
        int8_t actions[RED_MAX_GAP];
        int planned = 0;
        if (red_ok) {
          // RTP ts units per packet, derived from the actual stream (960 for
          // 20ms opus @48k) so a ptime change can't silently break mapping.
          uint32_t ts_step = (ts - seq_state[slot].last_ts) / (uint32_t)(delta + 1);
          planned = red_recover_plan(&red, delta, ts_step, actions);
        }
#ifdef PIPECAT_NACK
        // NACK lane (Phase 5): gaps of >= NACK_MIN_GAP (3) are beyond RED's
        // N-2 reach — re-request the OLDEST min(delta, 8) missing seqs over
        // the data channel. DEFERRED CONCEALMENT: the seqs we NACK emit
        // NOTHING here (the conceal loop below skips i < nacked); each one
        // resolves ~20ms later in nack_sweep_and_drain() as EITHER a spliced
        // rtx (real audio) or a deferred PLC signal — never both, so the ring
        // gets exactly one frame per lost packet (no time stretch). Gaps of
        // <= 2 never reach this (delta < NACK_MIN_GAP -> plan returns 0), so
        // the RED/FEC path below stays byte-identical for them. Frames past
        // the 8-seq cap (9..15 of a monster gap) keep immediate RED/PLC.
        int nacked = 0;
        if (delta >= NACK_MIN_GAP && s_nack_send != NULL &&
            rtp_decoder->on_packet == s_audio_on_packet) {
          uint16_t nack_seqs[NACK_MAX_SEQS];
          nacked = nack_client_plan_gap(expected, delta, nack_seqs,
                                        NACK_MAX_SEQS);
          if (nacked > 0) {
            nack_request(nack_seqs, nacked, nack_now_ms());
          }
        }
#endif
        for (int16_t i = 0; i < delta; i++) {
#ifdef PIPECAT_NACK
          if (i < nacked) {
            continue;  // deferred: resolved by rtx splice or sweep-PLC
          }
#endif
          if (i < planned && actions[i] >= 0) {
            RedBlock* b = &red.blocks[actions[i]];
            rtp_decoder->on_packet((uint8_t*)b->data, b->length, rtp_decoder->user_data);
            g_red_recovered++;
          } else {
            rtp_decoder->on_packet(NULL, 0, rtp_decoder->user_data);
          }
        }
      }
    }
    seq_state[slot].dec = rtp_decoder;
    seq_state[slot].last_seq = seq;
    seq_state[slot].last_ts = ts;
    seq_state[slot].initialized = 1;
  }
  // --- END VENDORED PATCH ---
  if (rtp_decoder->on_packet != NULL)
    rtp_decoder->on_packet(payload, payload_size, rtp_decoder->user_data);
  // even if there is no callback set, assume everything is ok for caller and do not return an error
  return (int)size;
}

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_decoder->on_packet = on_packet;
  rtp_decoder->user_data = user_data;

  switch (codec) {
    case CODEC_H264:
      rtp_decoder->decode_func = rtp_decode_h264;
      break;
    case CODEC_PCMA:
    case CODEC_PCMU:
    case CODEC_OPUS:
      rtp_decoder->decode_func = rtp_decode_generic;
      // Capture the audio decoder's consumer so an rtx arriving over the data
      // channel (rtp_nack_feed_rtx, outside any rtp_decode_generic call) can
      // be injected into the SAME decode path. One audio decoder per device.
      s_audio_on_packet = on_packet;
      s_audio_user_data = user_data;
    default:
      break;
  }
}

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (rtp_decoder->decode_func == NULL)
    return -1;
  return rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
}
