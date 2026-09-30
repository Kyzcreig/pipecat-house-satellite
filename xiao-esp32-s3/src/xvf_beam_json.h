#pragma once

#include <stddef.h>

struct PipecatXvfBeamTelemetry {
  float azimuth[4];
  float selected_azimuth[2];
  float spenergy[4];
  // AEC health scalars; NaN = read failed (serialized as JSON null).
  float rt60_s;
  float aec_converged;  // XMOS INT32 flag 0/1, carried as float
};

bool pipecat_xvf_beam_json(const PipecatXvfBeamTelemetry *telemetry, char *body,
                           size_t body_size);
