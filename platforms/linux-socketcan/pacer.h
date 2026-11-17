/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file pacer.h
 * @brief Wall-clock pacing for the SIL executables (soft real time).
 *
 * Simulated time is owned by the bench or the controller; the pacer only
 * slows a loop down to one iteration per period of wall-clock time.
 */

#ifndef OC_PLATFORM_PACER_H
#define OC_PLATFORM_PACER_H

#include <stdint.h>
#include <time.h>

typedef struct {
    struct timespec next;
    long period_ns;
    uint32_t overruns;  /**< iterations that started after their deadline */
} OcPacer;

void oc_pacer_init(OcPacer *p, uint32_t period_ms);

/** @brief Sleeps until the next deadline (absolute, so jitter does not accumulate). */
void oc_pacer_wait(OcPacer *p);

#endif /* OC_PLATFORM_PACER_H */
