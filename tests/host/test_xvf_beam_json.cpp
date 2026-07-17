#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../xiao-esp32-s3/src/xvf_beam_json.h"

static void test_serializes_finite_sample() {
  PipecatXvfBeamTelemetry telemetry = {
      {0.0f, 1.5f, -2.0f, 3.25f},
      {4.5f, 5.0f},
      {6.0f, 7.0f, 8.0f, 9.0f},
  };
  char body[384];

  assert(pipecat_xvf_beam_json(&telemetry, body, sizeof(body)));
  assert(strcmp(body,
                "{\"ok\":true,\"azimuth\":[0,1.5,-2,3.25],"
                "\"selected_azimuth\":[4.5,5],"
                "\"spenergy\":[6,7,8,9]}") == 0);
}

static void test_nonfinite_values_are_json_null() {
  PipecatXvfBeamTelemetry telemetry = {};
  telemetry.azimuth[1] = INFINITY;
  telemetry.selected_azimuth[0] = NAN;
  telemetry.spenergy[3] = -INFINITY;
  char body[384];

  assert(pipecat_xvf_beam_json(&telemetry, body, sizeof(body)));
  assert(strcmp(body,
                "{\"ok\":true,\"azimuth\":[0,null,0,0],"
                "\"selected_azimuth\":[null,0],"
                "\"spenergy\":[0,0,0,null]}") == 0);
}

static void test_rejects_short_output_buffer() {
  PipecatXvfBeamTelemetry telemetry = {};
  char body[16];

  assert(!pipecat_xvf_beam_json(&telemetry, body, sizeof(body)));
}

static void test_rejects_invalid_arguments() {
  PipecatXvfBeamTelemetry telemetry = {};
  char body[384];

  assert(!pipecat_xvf_beam_json(nullptr, body, sizeof(body)));
  assert(!pipecat_xvf_beam_json(&telemetry, nullptr, sizeof(body)));
  assert(!pipecat_xvf_beam_json(&telemetry, body, 0));
}

int main() {
  test_serializes_finite_sample();
  test_nonfinite_values_are_json_null();
  test_rejects_short_output_buffer();
  test_rejects_invalid_arguments();
  puts("xvf beam JSON host tests: PASS");
  return 0;
}
