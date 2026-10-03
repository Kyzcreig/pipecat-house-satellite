/* Host tests for components/peer/arrival_trace.c (t_5faf78b4). */
#include <stdio.h>
#include <stdlib.h>

#include "arrival_trace.h"

static int fails = 0;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

int main(void) {
  at_rec r;
  /* Uninitialised: push is a no-op, get fails. */
  at_push(1, 2, AT_ARR, 0);
  CHECK(at_get(0, &r) == 0);

  /* Non power-of-two capacity is rejected (stays disabled). */
  at_rec bad[6];
  at_init(bad, 6);
  at_push(1, 2, AT_ARR, 0);
  CHECK(at_get(0, &r) == 0);

  at_rec buf[8];
  at_init(buf, 8);
  CHECK(at_head() == 0);
  CHECK(at_get(0, &r) == 0); /* not written yet */
  at_push(100, 7, AT_ARR, 1);
  at_push(120, 8, AT_GAP, 3);
  CHECK(at_head() == 2);
  CHECK(at_get(0, &r) == 1 && r.t_ms == 100 && r.seq == 7 &&
        r.kind == AT_ARR && r.aux == 1);
  CHECK(at_get(1, &r) == 1 && r.t_ms == 120 && r.kind == AT_GAP &&
        r.aux == 3);
  CHECK(at_get(2, &r) == 0);

  /* Wrap: after 10 pushes total, indices 0,1 are overwritten by 8,9. */
  for (uint32_t i = 2; i < 10; i++) {
    at_push(100 + 20 * i, (uint16_t)(7 + i), AT_ARR, 0);
  }
  CHECK(at_head() == 10);
  CHECK(at_get(0, &r) == 0);
  CHECK(at_get(1, &r) == 0);
  CHECK(at_get(2, &r) == 1 && r.seq == 9);
  CHECK(at_get(9, &r) == 1 && r.seq == 16 && r.t_ms == 280);
  CHECK(at_get(10, &r) == 0);

  /* Re-init clears. */
  at_init(buf, 8);
  CHECK(at_head() == 0 && at_get(9, &r) == 0);

  if (fails) {
    fprintf(stderr, "%d FAILED\n", fails);
    return 1;
  }
  printf("arrival_trace: all tests passed\n");
  return 0;
}
