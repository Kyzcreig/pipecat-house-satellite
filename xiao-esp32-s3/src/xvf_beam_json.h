#pragma once

#include <stddef.h>

struct PipecatXvfBeamTelemetry {
  float azimuth[4];
  float selected_azimuth[2];
  float spenergy[4];
};

bool pipecat_xvf_beam_json(const PipecatXvfBeamTelemetry *telemetry, char *body,
                           size_t body_size);
