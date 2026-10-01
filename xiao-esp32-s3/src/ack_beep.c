/* ack_beep — see ack_beep.h for the contract. */
#include "ack_beep.h"

#include <math.h>
#include <string.h>

void ack_beep_init(ack_beep_state *s, int16_t *hist, uint32_t hist_cap) {
  memset(s, 0, sizeof(*s));
  s->hist = hist;
  s->hist_cap = hist_cap;
  s->best_corr_milli = -1000;
}

static void append(ack_beep_state *s, int16_t v) {
  if (s->hist != NULL && s->hist_cap > 0) {
    s->hist[s->pos % s->hist_cap] = v;
  }
  s->pos++;
}

static uint32_t oldest(const ack_beep_state *s) {
  return s->pos > s->hist_cap ? s->pos - s->hist_cap : 0;
}

/* Block features from history: rms, energy, tonal fraction at marker freqs. */
static void block_features(const ack_beep_state *s, uint32_t start, float *rms,
                           float *energy, float *tonal) {
  double e = 0.0;
  double tone = 0.0;
  double s1[ACK_BEEP_MAX_FREQS] = {0};
  double s2[ACK_BEEP_MAX_FREQS] = {0};
  double coef[ACK_BEEP_MAX_FREQS] = {0};
  const double two_pi = 6.283185307179586;
  for (int k = 0; k < s->m.nfreqs; k++) {
    coef[k] = 2.0 * cos(two_pi * (double)s->m.freqs[k] / (double)ACK_BEEP_RATE);
  }
  for (uint32_t i = 0; i < ACK_BEEP_BLOCK; i++) {
    double x = (double)s->hist[(start + i) % s->hist_cap];
    e += x * x;
    for (int k = 0; k < s->m.nfreqs; k++) {
      double s0 = x + coef[k] * s1[k] - s2[k];
      s2[k] = s1[k];
      s1[k] = s0;
    }
  }
  for (int k = 0; k < s->m.nfreqs; k++) {
    double p = s1[k] * s1[k] + s2[k] * s2[k] - coef[k] * s1[k] * s2[k];
    tone += p * 2.0 / (double)ACK_BEEP_BLOCK;
  }
  *energy = (float)e;
  *rms = (float)sqrt(e / (double)ACK_BEEP_BLOCK);
  *tonal = e > 0.0 ? (float)(tone / e) : 0.0f;
}

static float pearson(const float *x, const float *y, int n) {
  double mx = 0.0, my = 0.0;
  for (int i = 0; i < n; i++) {
    mx += x[i];
    my += y[i];
  }
  mx /= n;
  my /= n;
  double sxy = 0.0, sxx = 0.0, syy = 0.0;
  for (int i = 0; i < n; i++) {
    double dx = x[i] - mx, dy = y[i] - my;
    sxy += dx * dy;
    sxx += dx * dx;
    syy += dy * dy;
  }
  if (sxx <= 0.0 || syy <= 0.0)
    return 0.0f;
  return (float)(sxy / sqrt(sxx * syy));
}

static void resolve_played(ack_beep_state *s, float corr, float tonal,
                           uint32_t now_ms) {
  s->played++;
  memcpy(s->last_id, s->m.id, sizeof(s->last_id));
  s->last_ms = now_ms;
  s->last_corr_milli = (int32_t)lroundf(corr * 1000.0f);
  s->last_tonal_milli = (int32_t)lroundf(tonal * 1000.0f);
  s->consumed = s->scan_from;
  s->armed = 0;
}

static void resolve_missed(ack_beep_state *s) {
  s->missed++;
  memcpy(s->last_missed_id, s->m.id, sizeof(s->last_missed_id));
  s->armed = 0;
}

static void score_available(ack_beep_state *s, uint32_t now_ms) {
  const int n = s->m.env_len;
  while (s->armed && s->scan_from + ACK_BEEP_BLOCK <= s->pos) {
    if (s->scan_from < oldest(s)) {
      s->scan_from = oldest(s); /* history overrun: skip what is gone */
      continue;
    }
    float rms, energy, tonal;
    block_features(s, s->scan_from, &rms, &energy, &tonal);
    s->scan_from += ACK_BEEP_BLOCK;
    if (s->fcount == n) {
      memmove(s->frms, s->frms + 1, (size_t)(n - 1) * sizeof(float));
      memmove(s->fen, s->fen + 1, (size_t)(n - 1) * sizeof(float));
      memmove(s->ftone, s->ftone + 1, (size_t)(n - 1) * sizeof(float));
      s->fcount = n - 1;
    }
    s->frms[s->fcount] = rms;
    s->fen[s->fcount] = energy;
    s->ftone[s->fcount] = tonal;
    s->fcount++;
    if (s->fcount < n)
      continue;

    float corr = pearson(s->frms, s->m.env, n);
    double en = 0.0, wt = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
      en += s->fen[i];
      wt += (double)s->fen[i] * s->ftone[i];
      if (s->frms[i] > peak)
        peak = s->frms[i];
    }
    float wtonal = en > 0.0 ? (float)(wt / en) : 0.0f;
    int32_t cm = (int32_t)lroundf(corr * 1000.0f);
    if (cm > s->best_corr_milli)
      s->best_corr_milli = cm;
    if (corr >= ACK_BEEP_MIN_CORR &&
        wtonal >= ACK_BEEP_TONAL_FRAC * s->m.tonal &&
        peak >= ACK_BEEP_MIN_RMS) {
      resolve_played(s, corr, wtonal, now_ms);
      return;
    }
  }
}

void ack_beep_get_telemetry(const ack_beep_state *s, ack_beep_telemetry *t) {
  t->marks = s->marks;
  t->played = s->played;
  t->missed = s->missed;
  t->rejected = s->rejected;
  t->superseded = s->superseded;
  t->pending = s->armed;
  memcpy(t->last_id, s->last_id, sizeof(t->last_id));
  t->last_ms = s->last_ms;
  t->last_corr_milli = s->last_corr_milli;
  t->last_tonal_milli = s->last_tonal_milli;
  memcpy(t->last_missed_id, s->last_missed_id, sizeof(t->last_missed_id));
  t->best_corr_milli = s->best_corr_milli;
}

/* ids are echoed verbatim into JSON: allow only [A-Za-z0-9._-] */
static int id_valid(const char *id) {
  if (id[0] == '\0')
    return 0;
  for (const char *p = id; *p; p++) {
    char ch = *p;
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!ok)
      return 0;
  }
  return 1;
}

static int marker_valid(const ack_beep_marker *m) {
  if (memchr(m->id, '\0', ACK_BEEP_ID_LEN) == NULL)
    return 0;
  if (!id_valid(m->id))
    return 0;
  if (m->env_len < ACK_BEEP_MIN_ENV || m->env_len > ACK_BEEP_MAX_ENV)
    return 0;
  if (m->nfreqs < 1 || m->nfreqs > ACK_BEEP_MAX_FREQS)
    return 0;
  for (int k = 0; k < m->nfreqs; k++) {
    if (!(m->freqs[k] > 50.0f && m->freqs[k] < 7900.0f))
      return 0;
  }
  if (!(m->tonal >= ACK_BEEP_MIN_REF_TONAL && m->tonal <= 1.5f))
    return 0;
  float lo = m->env[0], hi = m->env[0];
  for (int i = 0; i < m->env_len; i++) {
    if (!(m->env[i] >= 0.0f) || m->env[i] > 40000.0f)
      return 0;
    if (m->env[i] < lo)
      lo = m->env[i];
    if (m->env[i] > hi)
      hi = m->env[i];
  }
  return hi > lo; /* a flat envelope has no shape to correlate */
}

ack_beep_arm_result ack_beep_arm(ack_beep_state *s, const ack_beep_marker *m,
                                 uint32_t now_ms) {
  s->marks++;
  if (!marker_valid(m)) {
    s->rejected++;
    return ACK_BEEP_REJECTED;
  }
  if (s->armed) {
    s->superseded++;
    resolve_missed(s);
  }
  s->m = *m;
  s->m.id[ACK_BEEP_ID_LEN - 1] = '\0';
  s->armed = 1;
  s->armed_ms = now_ms;
  s->fcount = 0;
  s->best_corr_milli = -1000;
  uint32_t from = oldest(s);
  if (s->consumed > from)
    from = s->consumed;
  s->scan_from = from;
  score_available(s, now_ms); /* audio may have beaten the marker */
  return ACK_BEEP_ARMED;
}

static void check_timeout(ack_beep_state *s, uint32_t now_ms) {
  if (s->armed && (uint32_t)(now_ms - s->armed_ms) >= ACK_BEEP_TIMEOUT_MS) {
    resolve_missed(s);
  }
}

void ack_beep_feed(ack_beep_state *s, const int16_t *pcm, uint32_t n,
                   uint32_t now_ms) {
  for (uint32_t i = 0; i < n; i++)
    append(s, pcm[i]);
  s->last_feed_ms = now_ms;
  s->pad_ms = 0;
  if (s->armed) {
    score_available(s, now_ms);
    check_timeout(s, now_ms);
  }
}

void ack_beep_tick(ack_beep_state *s, uint32_t now_ms) {
  if (!s->armed)
    return;
  uint32_t idle = now_ms - s->last_feed_ms;
  if (idle >= ACK_BEEP_IDLE_PAD_MS) {
    uint32_t cap_ms = (uint32_t)s->m.env_len * 20u;
    uint32_t want = idle < cap_ms ? idle : cap_ms;
    while (s->pad_ms + 20u <= want) {
      for (uint32_t i = 0; i < ACK_BEEP_BLOCK; i++)
        append(s, 0);
      s->pad_ms += 20u;
    }
    score_available(s, now_ms);
  }
  check_timeout(s, now_ms);
}
