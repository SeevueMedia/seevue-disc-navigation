#ifndef BDNAVD_BDJ_RETRY_H
#define BDNAVD_BDJ_RETRY_H
#include <stdint.h>

#define BDJ_RETRY_MS 5000
#define BDJ_RESTARTS 1
enum bdj_retry_action { BDJ_WAIT, BDJ_RESTART, BDJ_FAILED };

/* One budget for explicit start failures and the no-draw watchdog. The caller
   supplies monotonic time, so the rule can be tested without a JVM or sleeps. */
static inline enum bdj_retry_action bdj_retry_step(
    int *restarts, uint64_t *started_at, int start_failed, int playing, uint64_t now) {
  if (!start_failed) {
    if (playing) { *started_at = 0; return BDJ_WAIT; }
    if (!*started_at || now - *started_at < BDJ_RETRY_MS) return BDJ_WAIT;
  }
  if (*restarts >= BDJ_RESTARTS) { *started_at = 0; return BDJ_FAILED; }
  ++*restarts;
  *started_at = now;
  return BDJ_RESTART;
}
#endif
