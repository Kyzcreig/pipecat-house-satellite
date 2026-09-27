/* Host-side unit tests for decim_comp — the inverse-sinc droop compensator.
 *
 * Pure C, no ESP-IDF:
 *   ./tests/host/run_decim_comp_tests.sh    (from the repo root)
 *
 * Covers the properties that actually gate a flash on the audio hot path:
 *  - DC gain is EXACTLY unity in Q15 (sum of taps == 32768): the change must
 *    not shift the level the AGC/STT lane sees, or it silently confounds the
 *    A/B against the pre-change corpus.
 *  - Accumulator headroom: worst-case full-scale input cannot overflow int32.
 *  - Measured frequency response: it must actually LIFT 4/6/7.5 kHz by the
 *    amount the boxcar droops, i.e. the cascade is flat. A filter that
 *    compiles and runs but does not move the band is the failure mode that
 *    would otherwise reach a flash (test-gate-honesty).
 *  - Frame-boundary continuity: filtering a signal as one block and as
 *    consecutive 320-sample frames must produce IDENTICAL output. A per-frame
 *    state reset would inject a 50 Hz transient train.
 *  - Channel independence: the dual-stream lane carries two DIFFERENT XVF
 *    categories on L/R (cat-7 clean, cat-6 suppressed, D-41); a shared history
 *    would cross-talk them.
 *  - Clamping: a boost filter overshoots on full-scale transients; output must
 *    saturate, not wrap.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "decim_comp.h"

static int tests_run = 0;
#define RUN(fn)               \
  do {                        \
    fn();                     \
    tests_run++;              \
    printf("ok - %s\n", #fn); \
  } while (0)

#define SR 16000.0
#define PI 3.14159265358979323846

/* ── static properties ───────────────────────────────────────────────────── */

static void test_dc_gain_is_exactly_unity(void) {
  int32_t sum = 0;
  for (unsigned i = 0; i < DECIM_COMP_TAPS; i++)
    sum += kDecimCompTaps[i];
  assert(sum == 16384); /* Q14 unity: no level shift */
}

static void test_every_tap_fits_int16(void) {
  /* The compensator is a BOOST filter; in Q15 its centre tap would be 37782
   * and silently truncate. This is why the format is Q14. */
  for (unsigned i = 0; i < DECIM_COMP_TAPS; i++) {
    assert(kDecimCompTaps[i] >= -32768 && kDecimCompTaps[i] <= 32767);
  }
}

static void test_taps_are_symmetric(void) {
  for (unsigned i = 0; i < DECIM_COMP_TAPS / 2; i++) {
    assert(kDecimCompTaps[i] == kDecimCompTaps[DECIM_COMP_TAPS - 1 - i]);
  }
}

static void test_accumulator_cannot_overflow(void) {
  int64_t worst = 0;
  for (unsigned i = 0; i < DECIM_COMP_TAPS; i++) {
    int32_t t = kDecimCompTaps[i];
    worst += (int64_t)(t < 0 ? -t : t) * 32768; /* |int16| max magnitude */
  }
  assert(worst < 2147483647LL); /* fits int32 */
}

/* ── measured response (the gate that proves it does the job) ────────────── */

/* Boxcar-of-3 magnitude at 48 kHz evaluated at the 16 kHz-lane frequency f. */
static double boxcar3_db(double f) {
  double x = PI * f / 48000.0;
  if (f == 0.0)
    return 0.0;
  return 20.0 * log10(fabs(sin(3 * x) / (3 * sin(x))));
}

/* Drive the filter with a tone and measure output/input amplitude in dB.
 * Uses a long run and discards the transient, then takes the peak. */
static double measure_gain_db(double f) {
  static int16_t buf[16000];
  const unsigned n = 16000;
  const double amp = 8000.0; /* well inside clamp range */
  for (unsigned i = 0; i < n; i++) {
    buf[i] = (int16_t)lround(amp * sin(2 * PI * f * (double)i / SR));
  }
  decim_comp c;
  decim_comp_init(&c);
  decim_comp_run(&c, buf, n);
  int32_t peak = 0;
  for (unsigned i = 1000; i < n; i++) { /* skip the startup transient */
    int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
    if (v > peak)
      peak = v;
  }
  return 20.0 * log10((double)peak / amp);
}

static void test_cascade_is_flatter_than_the_boxcar(void) {
  /* The whole point of the change: boxcar + compensator ~= flat. */
  const double freqs[] = {1000, 2000, 4000, 6000, 7000, 7500};
  for (unsigned i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
    double f = freqs[i];
    double cascade = measure_gain_db(f) + boxcar3_db(f);
    double boxcar_alone = fabs(boxcar3_db(f));
    printf("    %5.0f Hz: cascade %+6.2f dB (boxcar alone %+6.2f dB)\n", f,
           cascade, boxcar3_db(f));
    assert(fabs(cascade) <
           0.75); /* design target 0.40 dB + measurement slack */
    /* And it must be a real improvement, not a wash: */
    assert(fabs(cascade) <= boxcar_alone + 0.05);
  }
}

static void test_it_actually_lifts_the_fricative_band(void) {
  /* Guard against an inert filter: 7.5 kHz must be boosted by ~3 dB. */
  double g = measure_gain_db(7500.0);
  printf("    compensator gain at 7500 Hz: %+.2f dB\n", g);
  assert(g > 2.3 && g < 3.8);
  /* ...while DC/low frequencies are untouched. */
  double lo = measure_gain_db(200.0);
  assert(fabs(lo) < 0.2);
}

/* ── streaming behaviour ─────────────────────────────────────────────────── */

static void test_frame_boundaries_are_seamless(void) {
  const unsigned frame = 320; /* PCM_SAMPLES_PER_FRAME */
  const unsigned n = frame * 5;
  static int16_t whole[1600], framed[1600];
  for (unsigned i = 0; i < n; i++) {
    int16_t v = (int16_t)lround(9000.0 * sin(2 * PI * 3000.0 * (double)i / SR));
    whole[i] = v;
    framed[i] = v;
  }
  decim_comp a, b;
  decim_comp_init(&a);
  decim_comp_init(&b);
  decim_comp_run(&a, whole, n);
  for (unsigned off = 0; off < n; off += frame) {
    decim_comp_run(&b, framed + off, frame);
  }
  assert(memcmp(whole, framed, n * sizeof(int16_t)) == 0);
}

static void test_stereo_channels_do_not_cross_talk(void) {
  /* L carries a tone, R carries silence. R must stay exactly silent. */
  const unsigned frames = 640;
  static int16_t x[1280];
  for (unsigned i = 0; i < frames; i++) {
    x[2 * i] = (int16_t)lround(12000.0 * sin(2 * PI * 5000.0 * (double)i / SR));
    x[2 * i + 1] = 0;
  }
  decim_comp l, r;
  decim_comp_init(&l);
  decim_comp_init(&r);
  decim_comp_run_stereo(&l, &r, x, frames);
  for (unsigned i = 0; i < frames; i++) {
    assert(x[2 * i + 1] == 0);
  }
  int32_t peak = 0;
  for (unsigned i = 10; i < frames; i++) {
    int32_t v = x[2 * i] < 0 ? -x[2 * i] : x[2 * i];
    if (v > peak)
      peak = v;
  }
  assert(peak > 12000); /* the L channel really was filtered (and boosted) */
}

static void test_stereo_matches_two_mono_runs(void) {
  const unsigned frames = 512;
  static int16_t inter[1024], lo[512], ro[512];
  for (unsigned i = 0; i < frames; i++) {
    int16_t a = (int16_t)lround(7000.0 * sin(2 * PI * 1200.0 * (double)i / SR));
    int16_t b = (int16_t)lround(5000.0 * sin(2 * PI * 6400.0 * (double)i / SR));
    inter[2 * i] = lo[i] = a;
    inter[2 * i + 1] = ro[i] = b;
  }
  decim_comp l, r, ml, mr;
  decim_comp_init(&l);
  decim_comp_init(&r);
  decim_comp_init(&ml);
  decim_comp_init(&mr);
  decim_comp_run_stereo(&l, &r, inter, frames);
  decim_comp_run(&ml, lo, frames);
  decim_comp_run(&mr, ro, frames);
  for (unsigned i = 0; i < frames; i++) {
    assert(inter[2 * i] == lo[i]);
    assert(inter[2 * i + 1] == ro[i]);
  }
}

static void test_output_saturates_never_wraps(void) {
  /* Full-scale square wave: a boost filter overshoots at every edge. */
  static int16_t x[2048];
  for (unsigned i = 0; i < 2048; i++)
    x[i] = (i / 8) % 2 ? 32767 : -32768;
  decim_comp c;
  decim_comp_init(&c);
  decim_comp_run(&c, x, 2048);
  for (unsigned i = 0; i < 2048; i++) {
    assert(x[i] >= -32768 && x[i] <= 32767);
  }
  /* A wrap would show as a sign flip right after an edge; saturation shows as
   * a run pinned at the rail. Assert at least one sample IS pinned (proof the
   * clamp is exercised by this stimulus, i.e. the test is not vacuous). */
  int pinned = 0;
  for (unsigned i = 0; i < 2048; i++) {
    if (x[i] == 32767 || x[i] == -32768)
      pinned = 1;
  }
  assert(pinned);
}

static void test_silence_stays_silent(void) {
  static int16_t x[640];
  memset(x, 0, sizeof(x));
  decim_comp c;
  decim_comp_init(&c);
  decim_comp_run(&c, x, 640);
  for (unsigned i = 0; i < 640; i++)
    assert(x[i] == 0);
}

int main(void) {
  RUN(test_dc_gain_is_exactly_unity);
  RUN(test_every_tap_fits_int16);
  RUN(test_taps_are_symmetric);
  RUN(test_accumulator_cannot_overflow);
  RUN(test_cascade_is_flatter_than_the_boxcar);
  RUN(test_it_actually_lifts_the_fricative_band);
  RUN(test_frame_boundaries_are_seamless);
  RUN(test_stereo_channels_do_not_cross_talk);
  RUN(test_stereo_matches_two_mono_runs);
  RUN(test_output_saturates_never_wraps);
  RUN(test_silence_stays_silent);
  printf("\n%d decim_comp tests passed\n", tests_run);
  return 0;
}
