// Host tests for the /xvf/route calibration policy (t_3565803e).
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "xvf_route_cal.h"

static int g_fail = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
      g_fail++;                                                  \
    }                                                            \
  } while (0)

int main() {
  // Category 3 (amplified mic) takes mic 0..3 only.
  for (unsigned src = 0; src <= 3; src++)
    CHECK(xvf_route_cal_valid(3, src));
  CHECK(!xvf_route_cal_valid(3, 4));
  // Category 4 (far-end reference) takes source 0 only.
  CHECK(xvf_route_cal_valid(4, 0));
  CHECK(!xvf_route_cal_valid(4, 1));
  // Never a beam, silence, user DSP, packed, out-of-range category, nor the
  // mic/reference categories the bench measured unusable through the ESP32
  // path (1, 11; 5, 12 unmeasured).
  for (unsigned cat :
       {0u, 1u, 2u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 255u}) {
    for (unsigned src = 0; src <= 5; src++)
      CHECK(!xvf_route_cal_valid(cat, src));
  }
  // Pair: dual stream takes two different routes; mono only the same route
  // twice (it averages both slots into one stream).
  CHECK(xvf_route_cal_pair_ok(3, 0, 3, 2, true));
  CHECK(!xvf_route_cal_pair_ok(3, 0, 3, 2, false));
  CHECK(xvf_route_cal_pair_ok(3, 2, 3, 2, false));
  CHECK(xvf_route_cal_pair_ok(4, 0, 4, 0, false));
  CHECK(!xvf_route_cal_pair_ok(3, 0, 7, 3, true));
  CHECK(!xvf_route_cal_pair_ok(7, 3, 3, 0, true));
  CHECK(XVF_ROUTE_PROD_L_CAT == 7 && XVF_ROUTE_PROD_L_SRC == 3);
  CHECK(XVF_ROUTE_PROD_R_CAT_DUAL == 6 && XVF_ROUTE_PROD_R_SRC == 3);

  // TTL: 0 = default, clamped to [min, max].
  CHECK(xvf_route_cal_ttl(0) == XVF_ROUTE_TTL_DEFAULT_MS);
  CHECK(xvf_route_cal_ttl(1) == XVF_ROUTE_TTL_MIN_MS);
  CHECK(xvf_route_cal_ttl(6000) == 6000);
  CHECK(xvf_route_cal_ttl(15000) == 15000);
  CHECK(xvf_route_cal_ttl(15001) == XVF_ROUTE_TTL_MAX_MS);
  CHECK(xvf_route_cal_ttl(0xFFFFFFFFu) == XVF_ROUTE_TTL_MAX_MS);

  // Parse.
  uint8_t c = 99, s = 99;
  CHECK(xvf_route_cal_parse("1,0", &c, &s) && c == 1 && s == 0);
  CHECK(xvf_route_cal_parse("11,3", &c, &s) && c == 11 && s == 3);
  for (const char *bad : {"", "1", "1,", ",1", "1,2,3", "a,1", "1, 2", "-1,0",
                          "100,0", "1,100", "1;0"}) {
    CHECK(!xvf_route_cal_parse(bad, &c, &s));
  }
  CHECK(!xvf_route_cal_parse(nullptr, &c, &s));

  if (g_fail) {
    std::printf("%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("xvf_route_cal: all checks pass\n");
  return 0;
}
