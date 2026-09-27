/* opus_gapfill — see opus_gapfill.h. */
#include "opus_gapfill.h"

/* libopus >= 1.5 opus_packet_has_lbrr(), re-derived on the public packet API
 * that the vendored esp-libopus (260b16c) exports. Semantics identical to
 * upstream src/opus_decoder.c: the SILK LBRR flag is the first range-coded
 * bit of the first frame's payload, after `nb_frames` VAD bits — one bit per
 * 20 ms SILK frame in the packet — and for a 2-channel stream the second
 * channel's LBRR flag follows its own VAD bits. Cross-checked packet-by-packet
 * against libopus 1.6.1 in tests/host/test_opus_gapfill.c. */
int opus_gapfill_packet_has_lbrr(const unsigned char *packet, int32_t len) {
  const unsigned char *frames[48];
  opus_int16 size[48];
  int nb_frames = 1;
  int lbrr;

  if (len < 1)
    return OPUS_BAD_ARG;
  /* TOC config >= 16 (top bit set) is CELT-only: no LBRR ever. */
  if (packet[0] & 0x80)
    return 0;
  int frame_size = opus_packet_get_samples_per_frame(packet, 48000);
  if (frame_size > 960)
    nb_frames = frame_size / 960;
  int stream_channels = opus_packet_get_nb_channels(packet);
  int ret = opus_packet_parse(packet, len, NULL, frames, size, NULL);
  if (ret <= 0)
    return ret < 0 ? ret : OPUS_INVALID_PACKET;
  if (size[0] < 1)
    return 0; /* empty SILK frame (DTX) carries nothing */
  lbrr = (frames[0][0] >> (7 - nb_frames)) & 0x1;
  if (stream_channels == 2)
    lbrr = lbrr || ((frames[0][0] >> (6 - 2 * nb_frames)) & 0x1);
  return lbrr;
}

int opus_gapfill_recover_one(OpusDecoder *dec, const unsigned char *data,
                             int32_t len, int16_t *pcm, int frame_size,
                             volatile uint32_t *fec, volatile uint32_t *plc) {
  /* Route on what the packet CARRIES, not on whether opus_decode "succeeded":
   * with decode_fec=1 libopus silently runs PLC for a no-LBRR packet and
   * returns a positive count, so the return value cannot attribute. `== 1`
   * on purpose — a malformed packet (<0) is not FEC either. If the FEC
   * decode itself fails, fall back to PLC so the ring still gets a frame. */
  if (opus_gapfill_packet_has_lbrr(data, len) == 1) {
    int n = opus_decode(dec, data, len, pcm, frame_size, 1 /* decode_fec */);
    if (n > 0) {
      (*fec)++;
      return n;
    }
  }
  (*plc)++;
  return opus_decode(dec, NULL, 0, pcm, frame_size, 0);
}
