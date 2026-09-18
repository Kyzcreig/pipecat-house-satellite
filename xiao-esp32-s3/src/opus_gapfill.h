/* opus_gapfill — downlink gap recovery (in-band FEC vs PLC) for the playback
 * path, factored out of media.cpp so it is host-testable against the SAME
 * vendored esp-libopus the firmware links (tests/host/run_gapfill_tests.sh).
 * Pure C, no ESP-IDF deps — same pattern as prebuffer_ctl.{c,h}.
 *
 * WHY THIS EXISTS (t_5730dda1, 2026-09-18): opus_decode(..., decode_fec=1)
 * does NOT fail on a packet with no LBRR. opus_decoder.c: "If no FEC can be
 * present, run the PLC (recursive call)" — it returns a positive sample count
 * of CONCEALMENT audio. So `fec_size > 0` cannot tell FEC from PLC, and the
 * `/playback/stats` fec counter was an upper bound: measured 61.6% of live
 * downlink packets (aiortc voip/fec=1/packet_loss=10) carry no LBRR, and every
 * one of them was being credited as "recovered via in-band FEC".
 *
 * The vendored esp-libopus (260b16c, 2020) predates libopus 1.5's
 * opus_packet_has_lbrr(); opus_gapfill_packet_has_lbrr() is that function
 * re-derived on the public packet API this tree DOES export, cross-checked
 * packet-by-packet against libopus 1.6.1 in the host test.
 */
#ifndef OPUS_GAPFILL_H
#define OPUS_GAPFILL_H

#include <stddef.h>
#include <stdint.h>

#include <opus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = the packet carries LBRR (in-band FEC for the PREVIOUS frame),
 * 0 = it does not (incl. CELT-only packets, which never do),
 * <0 = malformed packet (an opus error code). Same contract as libopus>=1.5
 * opus_packet_has_lbrr(); callers must test `== 1`, never truthiness. */
int opus_gapfill_packet_has_lbrr(const unsigned char *packet, int32_t len);

/* Recover ONE lost frame that immediately preceded `data` (the next packet
 * to arrive). Decodes into `pcm` (frame_size samples of the decoder's channel
 * count) and increments exactly one of *fec / *plc:
 *   - packet carries LBRR  -> opus_decode(data, decode_fec=1): REAL audio, *fec++
 *   - otherwise            -> opus_decode(NULL) PLC synthesis,           *plc++
 * Returns the decoded sample count (<=0 on decoder error; counters are still
 * attributed by the LBRR verdict, so a failed FEC decode is not a PLC). */
int opus_gapfill_recover_one(OpusDecoder *dec, const unsigned char *data,
                             int32_t len, int16_t *pcm, int frame_size,
                             volatile uint32_t *fec, volatile uint32_t *plc);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_GAPFILL_H */
