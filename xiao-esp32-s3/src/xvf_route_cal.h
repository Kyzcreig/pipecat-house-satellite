#pragma once

#include <stdint.h>

// Temporary XVF3800 output routing for microphone calibration (t_3565803e).
// Compiled in only with PIPECAT_XVF_ROUTE_CAL=1 (default 0, never on the fleet
// image). Transport-free so the host tests can drive the policy.
//
// The two I2S output slots are set by AUDIO_MGR_OP_L / AUDIO_MGR_OP_R (resid 35,
// cmd 15 / 19), each a (category, source) pair, volatile on the chip. The boot
// profile (configure_xvf3800_dsp_profile) writes the production pair:
//   L = (7, 3) ASR auto-select beam    -> the hub's clean lane
//   R = (6, 3) processed auto-select   -> the hub's sup lane (dual stream)
// A calibration route is allowed only to the categories that carry ONE
// microphone or the far-end reference, never a beam:
//   1  raw mic before amplification (mic_gain)   source 0..3
//   3  amplified mic, system delay applied        source 0..3
//   11 amplified mic, before system delay         source 0..3
//   4  far-end reference (16 kHz)                 source 0
//   5  far-end reference, system delay            source 0
//   12 amplified far-end, system delay            source 0
// Every calibration route carries a TTL (clamped); when it ends, on any write or
// readback failure, and on reboot (the boot profile rewrites both slots) the
// production pair is restored.

static constexpr uint8_t XVF_ROUTE_PROD_L_CAT = 7;
static constexpr uint8_t XVF_ROUTE_PROD_L_SRC = 3;
static constexpr uint8_t XVF_ROUTE_PROD_R_CAT_DUAL = 6;
static constexpr uint8_t XVF_ROUTE_PROD_R_CAT_MONO = 7;
static constexpr uint8_t XVF_ROUTE_PROD_R_SRC = 3;

static constexpr uint32_t XVF_ROUTE_TTL_DEFAULT_MS = 8000;
static constexpr uint32_t XVF_ROUTE_TTL_MIN_MS = 500;
static constexpr uint32_t XVF_ROUTE_TTL_MAX_MS = 15000;

static inline bool xvf_route_cal_valid(uint8_t category, uint8_t source) {
  switch (category) {
    case 1:
    case 3:
    case 11:
      return source <= 3;
    case 4:
    case 5:
    case 12:
      return source == 0;
    default:
      return false;
  }
}

static inline uint32_t xvf_route_cal_ttl(uint32_t requested_ms) {
  if (requested_ms == 0) return XVF_ROUTE_TTL_DEFAULT_MS;
  if (requested_ms < XVF_ROUTE_TTL_MIN_MS) return XVF_ROUTE_TTL_MIN_MS;
  if (requested_ms > XVF_ROUTE_TTL_MAX_MS) return XVF_ROUTE_TTL_MAX_MS;
  return requested_ms;
}

// Parse "<cat>,<src>" (decimal, no spaces). Returns false on anything else.
static inline bool xvf_route_cal_parse(const char *s, uint8_t *category,
                                       uint8_t *source) {
  if (s == nullptr || category == nullptr || source == nullptr) return false;
  unsigned vals[2] = {0, 0};
  int idx = 0;
  int digits = 0;
  for (const char *p = s;; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      vals[idx] = vals[idx] * 10u + static_cast<unsigned>(c - '0');
      if (++digits > 2) return false;
    } else if (c == ',' && idx == 0 && digits > 0) {
      idx = 1;
      digits = 0;
    } else if (c == '\0' && idx == 1 && digits > 0) {
      break;
    } else {
      return false;
    }
  }
  *category = static_cast<uint8_t>(vals[0]);
  *source = static_cast<uint8_t>(vals[1]);
  return true;
}
