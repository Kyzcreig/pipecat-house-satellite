/* Host test: downlink gap-fill must credit `fec` ONLY when the next packet
 * actually carries LBRR, and `plc` otherwise (t_5730dda1).
 *
 * Links the VENDORED esp-libopus (esp32-s3-box-3/components/esp-libopus, the
 * exact tree the firmware links, FIXED_POINT) and decodes a REAL live-config
 * downlink stream (fixtures/downlink/, see PROVENANCE.txt) in the firmware's
 * exact decoder configuration: opus_decoder_create(16000, 1), 320-sample
 * frames. Each packet's LBRR truth comes from libopus 1.6.1
 * opus_packet_has_lbrr() recorded at fixture-generation time (.lbrr file).
 *
 * Test 1 (the RED-proof): for every packet i>0, feed packets [0..i-1]
 * normally, declare packet i-1 LOST, then hand packet i to the gap-fill.
 * Expected: fec++ iff oracle[i]==1, plc++ otherwise. Today's firmware logic
 * credits fec for EVERY packet (opus_decode(decode_fec=1) never fails).
 *
 * Test 2: the firmware-side LBRR parser agrees with the oracle on every
 * packet of the stream (and returns 0, never 1, for a CELT-only TOC).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#include "opus_gapfill.h"

#define FS 16000
#define FRAME 320 /* PCM_SAMPLES_PER_FRAME: 20 ms @ 16 kHz */
#define MAX_PKTS 1024

static int g_fail = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      g_fail++;                                                   \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
      fprintf(stderr, __VA_ARGS__);                               \
      fputc('\n', stderr);                                        \
    }                                                             \
  } while (0)

static unsigned char *g_pkt[MAX_PKTS];
static int g_len[MAX_PKTS];
static unsigned char g_oracle[MAX_PKTS];
static int g_n = 0;

static void load(const char *dir) {
  char path[1024];
  snprintf(path, sizeof path, "%s/downlink_stream.bin", dir);
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  for (;;) {
    unsigned char hdr[2];
    if (fread(hdr, 1, 2, f) != 2) break;
    int len = hdr[0] | (hdr[1] << 8);
    if (g_n >= MAX_PKTS) { fprintf(stderr, "too many packets\n"); exit(2); }
    g_pkt[g_n] = malloc(len);
    if (fread(g_pkt[g_n], 1, len, f) != (size_t)len) { fprintf(stderr, "short read\n"); exit(2); }
    g_len[g_n] = len;
    g_n++;
  }
  fclose(f);
  snprintf(path, sizeof path, "%s/downlink_stream.lbrr", dir);
  f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  if (fread(g_oracle, 1, g_n, f) != (size_t)g_n) { fprintf(stderr, "oracle length mismatch\n"); exit(2); }
  fclose(f);
}

static int peak(const int16_t *pcm, int n) {
  int p = 0;
  for (int i = 0; i < n; i++) { int a = abs(pcm[i]); if (a > p) p = a; }
  return p;
}

static void test_gapfill_attribution(void) {
  int err = 0;
  int16_t pcm[FRAME];
  int with = 0, without = 0, fec_wrong = 0, plc_wrong = 0;
  int silent_fec = 0;
  for (int i = 1; i < g_n; i++) {
    OpusDecoder *dec = opus_decoder_create(FS, 1, &err);
    CHECK(err == OPUS_OK, "decoder create %d", err);
    for (int j = 0; j < i - 1; j++) {
      int n = opus_decode(dec, g_pkt[j], g_len[j], pcm, FRAME, 0);
      CHECK(n == FRAME, "pkt %d normal decode -> %d", j, n);
    }
    /* packet i-1 is LOST; packet i arrives and must fill the gap. */
    volatile uint32_t fec = 0, plc = 0;
    int n = opus_gapfill_recover_one(dec, g_pkt[i], g_len[i], pcm, FRAME, &fec, &plc);
    CHECK(n == FRAME, "pkt %d gapfill -> %d samples", i, n);
    CHECK(fec + plc == 1, "pkt %d: exactly one counter must move (fec=%u plc=%u)", i, fec, plc);
    if (g_oracle[i] == 1) {
      with++;
      if (fec != 1) fec_wrong++;
      CHECK(fec == 1 && plc == 0, "pkt %d HAS LBRR (oracle) but fec=%u plc=%u", i, fec, plc);
    } else {
      without++;
      if (plc != 1) plc_wrong++;
      if (fec == 1 && peak(pcm, n) == 0) silent_fec++;
      CHECK(plc == 1 && fec == 0, "pkt %d has NO LBRR (oracle) but fec=%u plc=%u", i, fec, plc);
    }
    opus_decoder_destroy(dec);
  }
  printf("gapfill attribution: packets=%d with_LBRR=%d without_LBRR=%d "
         "fec_misattributed=%d plc_misattributed=%d silent_frames_credited_as_fec=%d\n",
         g_n - 1, with, without, fec_wrong, plc_wrong, silent_fec);
  CHECK(with > 10 && without > 10, "fixture must exercise BOTH classes (with=%d without=%d)", with, without);
}

static void test_lbrr_parser_matches_oracle(void) {
  int mismatches = 0;
  for (int i = 0; i < g_n; i++) {
    int v = opus_gapfill_packet_has_lbrr(g_pkt[i], g_len[i]);
    if (v != (int)g_oracle[i]) mismatches++;
    CHECK(v == (int)g_oracle[i], "pkt %d toc=0x%02x len=%d: parser=%d oracle=%d",
          i, g_pkt[i][0], g_len[i], v, g_oracle[i]);
  }
  printf("lbrr parser vs libopus-1.6.1 oracle: packets=%d mismatches=%d\n", g_n, mismatches);

  /* CELT-only TOC (config >= 16, i.e. top bit set) never carries LBRR. */
  unsigned char celt[3] = {0xF8, 0x00, 0x00};
  CHECK(opus_gapfill_packet_has_lbrr(celt, sizeof celt) == 0, "CELT-only must be 0");
  /* Malformed: a code-3 packet with no frame-count byte -> negative, never 1. */
  unsigned char bad[1] = {0x63};
  CHECK(opus_gapfill_packet_has_lbrr(bad, sizeof bad) < 0, "malformed must be <0");
  CHECK(opus_gapfill_packet_has_lbrr(bad, 0) < 0, "zero-length must be <0");
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]); return 2; }
  load(argv[1]);
  printf("vendored %s, %d packets\n", opus_get_version_string(), g_n);
  test_gapfill_attribution();
  test_lbrr_parser_matches_oracle();
  if (g_fail) { printf("FAILED (%d checks)\n", g_fail); return 1; }
  printf("ok\n");
  return 0;
}
