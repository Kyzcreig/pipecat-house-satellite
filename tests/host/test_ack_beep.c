/* Host-side unit tests for ack_beep (t_69ffa409): the satellite counts the
 * hub's wake ACK beep as PLAYED only when audio actually written to I2S
 * matches the marker fingerprint.
 *
 *   ./tests/host/run_ack_beep_tests.sh   (from the repo root)
 *
 * The marker below is exactly what the hub's
 * server/processors/ack_beep_marker.py computes for the deployed chime
 * (wake_word_triggered_200ms_m10db.pcm, copied byte-exact into
 * tests/host/fixtures/ack_chime_200ms_m10db.pcm).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ack_beep.h"

static int tests_run = 0;
#define RUN(fn)               \
  do {                        \
    fn();                     \
    tests_run++;              \
    printf("ok - %s\n", #fn); \
  } while (0)

#define HIST 9600 /* 600 ms lookback, same as media.cpp */
static int16_t hist[HIST];
static int16_t *chime;
static uint32_t chime_n;
static int16_t *speech;
static uint32_t speech_n;

static int16_t *load(const char *path, uint32_t *n) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  int16_t *buf = (int16_t *)malloc((size_t)len);
  if (fread(buf, 1, (size_t)len, f) != (size_t)len)
    exit(2);
  fclose(f);
  *n = (uint32_t)(len / 2);
  return buf;
}

static ack_beep_marker chime_marker(const char *id) {
  static const float env[10] = {211.5f,  841.1f,  1807.9f, 2336.3f, 2396.2f,
                                3019.7f, 2775.3f, 1422.1f, 449.3f,  153.2f};
  ack_beep_marker m;
  memset(&m, 0, sizeof(m));
  snprintf(m.id, sizeof(m.id), "%s", id);
  memcpy(m.env, env, sizeof(env));
  m.env_len = 10;
  m.freqs[0] = 795.0f;
  m.freqs[1] = 942.0f;
  m.nfreqs = 2;
  m.tonal = 0.722f;
  return m;
}

/* Feed like the playback task: 320-sample frames, 20 ms apart. */
static uint32_t feed_frames(ack_beep_state *s, const int16_t *pcm, uint32_t n,
                            uint32_t now_ms, float gain) {
  int16_t frame[ACK_BEEP_BLOCK];
  for (uint32_t i = 0; i < n; i += ACK_BEEP_BLOCK) {
    for (uint32_t j = 0; j < ACK_BEEP_BLOCK; j++) {
      float v = (i + j < n) ? pcm[i + j] * gain : 0.0f;
      frame[j] = (int16_t)lroundf(v);
    }
    ack_beep_feed(s, frame, ACK_BEEP_BLOCK, now_ms);
    now_ms += 20;
  }
  return now_ms;
}

static uint32_t feed_silence(ack_beep_state *s, uint32_t ms, uint32_t now_ms) {
  static int16_t zero[ACK_BEEP_BLOCK];
  for (uint32_t t = 0; t < ms; t += 20) {
    ack_beep_feed(s, zero, ACK_BEEP_BLOCK, now_ms);
    now_ms += 20;
  }
  return now_ms;
}

/* chime shifted by `pre` samples of other audio (block misalignment) */
static uint32_t feed_chime_shifted(ack_beep_state *s, uint32_t pre,
                                   uint32_t now_ms) {
  uint32_t n = pre + chime_n + 640;
  int16_t *buf = (int16_t *)calloc(n, sizeof(int16_t));
  memcpy(buf + pre, chime, chime_n * sizeof(int16_t));
  now_ms = feed_frames(s, buf, n, now_ms, 1.0f);
  free(buf);
  return now_ms;
}

static void test_marker_then_chime_is_played_at_every_alignment(void) {
  const uint32_t shifts[] = {0, 80, 160, 240};
  for (size_t k = 0; k < 4; k++) {
    ack_beep_state s;
    ack_beep_init(&s, hist, HIST);
    ack_beep_marker m = chime_marker("wb-1");
    assert(ack_beep_arm(&s, &m, 1000) == ACK_BEEP_ARMED);
    feed_chime_shifted(&s, shifts[k], 1010);
    assert(s.played == 1);
    assert(s.missed == 0);
    assert(strcmp(s.last_id, "wb-1") == 0);
    assert(s.last_corr_milli >= 850);
    assert(s.last_tonal_milli >= 361);
    assert(s.armed == 0);
  }
}

static void test_chime_before_marker_is_matched_from_history(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  uint32_t now = feed_frames(&s, chime, chime_n, 1000, 1.0f);
  ack_beep_marker m = chime_marker("wb-early");
  ack_beep_arm(&s, &m, now + 60);
  assert(s.played == 1);
  assert(strcmp(s.last_id, "wb-early") == 0);
}

static void test_scaled_chime_still_matches(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker m = chime_marker("wb-quiet");
  ack_beep_arm(&s, &m, 0);
  uint32_t now = feed_frames(&s, chime, chime_n, 10, 0.25f);
  feed_silence(&s, 200, now);
  assert(s.played == 1);
}

static void test_unrequested_chime_never_counts(void) {
  /* the acceptance negative control: the real chime with NO marker */
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  uint32_t now = feed_frames(&s, chime, chime_n, 0, 1.0f);
  now = feed_silence(&s, 400, now);
  ack_beep_tick(&s, now + 5000);
  assert(s.played == 0);
  assert(s.marks == 0);
  assert(s.missed == 0);
}

static void test_marker_with_speech_audio_is_missed(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker m = chime_marker("wb-speech");
  ack_beep_arm(&s, &m, 0);
  uint32_t now = feed_frames(&s, speech, speech_n, 10, 1.0f);
  ack_beep_tick(&s, now + ACK_BEEP_TIMEOUT_MS);
  assert(s.played == 0);
  assert(s.missed == 1);
  assert(strcmp(s.last_missed_id, "wb-speech") == 0);
}

static void test_marker_with_wrong_tone_is_missed(void) {
  /* the hub's synth fallback (880 Hz sine, 160 ms) is not this chime */
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  uint32_t n = 16000 * 160 / 1000;
  int16_t *sine = (int16_t *)calloc(n, sizeof(int16_t));
  uint32_t fade = (uint32_t)(n * 0.15);
  for (uint32_t i = 0; i < n; i++) {
    double amp = 0.35;
    if (i < fade)
      amp *= (double)i / fade;
    else if (i > n - fade)
      amp *= (double)(n - i) / fade;
    sine[i] = (int16_t)(amp * 32767 * sin(2 * M_PI * 880.0 * i / 16000.0));
  }
  ack_beep_marker m = chime_marker("wb-sine");
  ack_beep_arm(&s, &m, 0);
  uint32_t now = feed_frames(&s, sine, n, 10, 1.0f);
  now = feed_silence(&s, 300, now);
  ack_beep_tick(&s, now + ACK_BEEP_TIMEOUT_MS);
  assert(s.played == 0);
  assert(s.missed == 1);
  free(sine);
}

static void test_marker_with_no_audio_times_out(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker m = chime_marker("wb-silent");
  ack_beep_arm(&s, &m, 100);
  ack_beep_tick(&s, 100 + ACK_BEEP_TIMEOUT_MS - 1);
  assert(s.armed == 1 && s.missed == 0);
  ack_beep_tick(&s, 100 + ACK_BEEP_TIMEOUT_MS);
  assert(s.armed == 0 && s.missed == 1 && s.played == 0);
}

static void test_chime_ending_the_stream_is_padded_and_played(void) {
  /* the ring drains right after the beep: nothing more is written */
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker m = chime_marker("wb-tail");
  ack_beep_arm(&s, &m, 0);
  uint32_t now = feed_frames(&s, chime + 160, chime_n - 160, 10, 1.0f);
  assert(s.played == 0 || s.played == 1);
  for (uint32_t t = now; t < now + 400; t += 5)
    ack_beep_tick(&s, t);
  assert(s.played == 1);
}

static void test_one_chime_cannot_satisfy_two_markers(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker a = chime_marker("wb-a");
  ack_beep_arm(&s, &a, 0);
  uint32_t now = feed_frames(&s, chime, chime_n, 10, 1.0f);
  now = feed_silence(&s, 100, now);
  assert(s.played == 1);
  ack_beep_marker b = chime_marker("wb-b");
  ack_beep_arm(&s, &b, now);
  assert(s.played == 1);
  ack_beep_tick(&s, now + ACK_BEEP_TIMEOUT_MS);
  assert(s.missed == 1 && strcmp(s.last_missed_id, "wb-b") == 0);
}

static void test_superseded_marker_is_missed(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  ack_beep_marker a = chime_marker("wb-a");
  ack_beep_marker b = chime_marker("wb-b");
  ack_beep_arm(&s, &a, 0);
  ack_beep_arm(&s, &b, 50);
  assert(s.superseded == 1 && s.missed == 1);
  feed_frames(&s, chime, chime_n, 60, 1.0f);
  assert(s.played == 1 && strcmp(s.last_id, "wb-b") == 0);
  assert(s.marks == 2);
}

static void test_chime_older_than_lookback_is_not_matched(void) {
  ack_beep_state s;
  ack_beep_init(&s, hist, HIST);
  uint32_t now = feed_frames(&s, chime, chime_n, 0, 1.0f);
  now = feed_silence(&s, 700, now); /* > 600 ms of later audio */
  ack_beep_marker m = chime_marker("wb-late");
  ack_beep_arm(&s, &m, now);
  ack_beep_tick(&s, now + ACK_BEEP_TIMEOUT_MS);
  assert(s.played == 0 && s.missed == 1);
}

static void test_invalid_markers_are_rejected_and_never_count(void) {
  ack_beep_marker bad[6];
  bad[0] = chime_marker("");        /* no id */
  bad[1] = chime_marker("wb-flat"); /* flat envelope */
  for (int i = 0; i < 10; i++)
    bad[1].env[i] = 1000.0f;
  bad[2] = chime_marker("wb-nontonal");
  bad[2].tonal = 0.05f;
  bad[3] = chime_marker("wb-short");
  bad[3].env_len = 2;
  bad[4] = chime_marker("wb-badfreq");
  bad[4].freqs[0] = 0.0f;
  bad[5] = chime_marker("wb\",\"x"); /* echoed into JSON: must be safe */
  for (int k = 0; k < 6; k++) {
    ack_beep_state s;
    ack_beep_init(&s, hist, HIST);
    assert(ack_beep_arm(&s, &bad[k], 0) == ACK_BEEP_REJECTED);
    feed_frames(&s, chime, chime_n, 10, 1.0f);
    assert(s.played == 0 && s.rejected == 1 && s.marks == 1 && s.armed == 0);
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <chime.pcm> <speech.pcm>\n", argv[0]);
    return 2;
  }
  chime = load(argv[1], &chime_n);
  speech = load(argv[2], &speech_n);
  RUN(test_marker_then_chime_is_played_at_every_alignment);
  RUN(test_chime_before_marker_is_matched_from_history);
  RUN(test_scaled_chime_still_matches);
  RUN(test_unrequested_chime_never_counts);
  RUN(test_marker_with_speech_audio_is_missed);
  RUN(test_marker_with_wrong_tone_is_missed);
  RUN(test_marker_with_no_audio_times_out);
  RUN(test_chime_ending_the_stream_is_padded_and_played);
  RUN(test_one_chime_cannot_satisfy_two_markers);
  RUN(test_superseded_marker_is_missed);
  RUN(test_chime_older_than_lookback_is_not_matched);
  RUN(test_invalid_markers_are_rejected_and_never_count);
  printf("1..%d ack_beep tests passed\n", tests_run);
  return 0;
}
