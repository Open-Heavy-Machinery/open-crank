/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file l2_accept.h
 * @brief Level 2 observables for the acceptance oracle (open-tick oracle.h).
 *
 * At Level 2 the criteria are about the engine, not only the bus: the plant's
 * real speed and where the injections land. The oracle reads frames only, so
 * the runner hands it, every step, one observation frame per plant
 * observable: an 11-bit frame OC_L2_OBS_ID + index carrying the value as a
 * little-endian IEEE double (bytes 0-7), or no payload for a gap. They exist
 * only between the runner and the oracle: never on a bus, never in a frame
 * log. J1939 uses 29-bit identifiers only, so they cannot be mistaken for a
 * real frame. ecu_rpm is read from the ECU's own EEC1. A hard run (step 7)
 * computes the same plant observables from the same plant.
 *
 * | Observable            | Value                                                              | Default |
 * |-----------------------|--------------------------------------------------------------------|---------|
 * | `plant_rpm`           | the plant's crankshaft speed                                       | 0       |
 * | `ecu_rpm`             | the speed in the ECU's EEC1 (its own clock)                        | gap     |
 * | `misplaced`           | injections outside every window or on another cylinder's channel, since the start | 0 |
 * | `misfires_1s`         | windows that closed with no injection, in the last second          | 0       |
 * | `first_injection_rev` | crank revolutions from rest to the first injection counted; a gap before it | gap |
 * | `soi_err_deg`         | the last counted injection's start minus its commanded angle       | gap     |
 * | `fuel_pct`            | the fuel the plant burns: the mean of its six cylinders            | 0       |
 */

#ifndef OPEN_CRANK_L2_ACCEPT_H
#define OPEN_CRANK_L2_ACCEPT_H

#include <stddef.h>

#include <opencrank/l2/l2_loop.h>
#include <opentick/oracle.h>

#define OC_L2_OBS_ID 0x7F0U

typedef enum {
    OC_L2_OBS_PLANT_RPM = 0,
    OC_L2_OBS_ECU_RPM,
    OC_L2_OBS_MISPLACED,
    OC_L2_OBS_MISFIRES_1S,
    OC_L2_OBS_FIRST_INJECTION_REV,
    OC_L2_OBS_SOI_ERR_DEG,
    OC_L2_OBS_FUEL_PCT,
    OC_L2_OBS_COUNT
} OcL2Observable;

extern const OtkOracleObservable OC_L2_OBSERVABLES[OC_L2_OBS_COUNT];

/** @brief The oracle's table for Level 2; the ECU is at @p ecu_address. */
void oc_l2_accept_init(OtkOracleTable *table, const uint8_t *ecu_address);

/** @brief The observation frames of the loop's present state (every observable but ecu_rpm). */
size_t oc_l2_observation_frames(const OcL2Loop *l, OtkCanFrame *out, size_t cap);

#endif /* OPEN_CRANK_L2_ACCEPT_H */
