/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file pacer.c
 * @brief Wall-clock pacing for the SIL executables. See pacer.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "pacer.h"

#define NS_PER_S 1000000000L

void oc_pacer_init(OcPacer *p, uint32_t period_ms) {
    p->period_ns = (long)period_ms * 1000000L;
    p->overruns = 0U;
    clock_gettime(CLOCK_MONOTONIC, &p->next);
}

void oc_pacer_wait(OcPacer *p) {
    p->next.tv_nsec += p->period_ns;
    while (p->next.tv_nsec >= NS_PER_S) {
        p->next.tv_sec += 1;
        p->next.tv_nsec -= NS_PER_S;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > p->next.tv_sec || (now.tv_sec == p->next.tv_sec && now.tv_nsec > p->next.tv_nsec)) {
        p->overruns++;
        p->next = now; /* do not try to catch up with a burst of steps */
        return;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &p->next, NULL) != 0) {
        /* interrupted by a signal: sleep again until the deadline */
    }
}
