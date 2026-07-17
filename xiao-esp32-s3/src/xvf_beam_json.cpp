#include "xvf_beam_json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static bool append_json_float(char *body, size_t body_size, size_t *used,
                              float value, const char *suffix) {
  int written = std::isfinite(value)
                    ? snprintf(body + *used, body_size - *used, "%.9g%s",
                               static_cast<double>(value), suffix)
                    : snprintf(body + *used, body_size - *used, "null%s", suffix);
  if (written < 0 || static_cast<size_t>(written) >= body_size - *used) {
    return false;
  }
  *used += static_cast<size_t>(written);
  return true;
}

bool pipecat_xvf_beam_json(const PipecatXvfBeamTelemetry *telemetry, char *body,
                           size_t body_size) {
  if (telemetry == nullptr || body == nullptr || body_size == 0) {
    return false;
  }
  int written = snprintf(body, body_size, "{\"ok\":true,\"azimuth\":[");
  if (written < 0 || static_cast<size_t>(written) >= body_size) {
    return false;
  }

  size_t used = static_cast<size_t>(written);
  bool fit = true;
  fit = append_json_float(body, body_size, &used, telemetry->azimuth[0], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->azimuth[1], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->azimuth[2], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->azimuth[3],
                          "],\"selected_azimuth\":[") && fit;
  fit = append_json_float(body, body_size, &used,
                          telemetry->selected_azimuth[0], ",") && fit;
  fit = append_json_float(body, body_size, &used,
                          telemetry->selected_azimuth[1],
                          "],\"spenergy\":[") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->spenergy[0], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->spenergy[1], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->spenergy[2], ",") && fit;
  fit = append_json_float(body, body_size, &used, telemetry->spenergy[3], "]}") && fit;
  return fit;
}
