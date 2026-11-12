#include "../src/bdj_retry.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  int retries = 0;
  uint64_t started = 1;
  assert(bdj_retry_step(&retries, &started, 0, 0, 5000) == BDJ_WAIT);
  assert(bdj_retry_step(&retries, &started, 0, 0, 5001) == BDJ_RESTART);
  assert(retries == 1);
  assert(bdj_retry_step(&retries, &started, 0, 1, 5002) == BDJ_WAIT);
  assert(!started); /* first failure, restart succeeds */
  assert(bdj_retry_step(&retries, &started, 1, 1, 5003) == BDJ_FAILED);
  assert(retries == 1); /* same session never acquires another retry */

  retries = 0; started = 1;
  assert(bdj_retry_step(&retries, &started, 1, 0, 2) == BDJ_RESTART);
  assert(bdj_retry_step(&retries, &started, 0, 0, 5002) == BDJ_FAILED);
  assert(!started && retries == 1); /* explicit failure then no draw */
  assert(bdj_retry_step(&retries, &started, 0, 0, 10002) == BDJ_WAIT);

  retries = 0; started = 1;
  assert(bdj_retry_step(&retries, &started, 0, 0, 5001) == BDJ_RESTART);
  assert(bdj_retry_step(&retries, &started, 0, 0, 10001) == BDJ_FAILED);
  assert(retries == 1 && !started); /* two silent starts end with a reason */
  puts("BD-J retry policy passed");
  return 0;
}
