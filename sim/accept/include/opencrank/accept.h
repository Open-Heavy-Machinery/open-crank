/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file accept.h
 * @brief open-crank's observables for the acceptance oracle: what the
 *        device under test asks of the engine and of the hoist pump, read
 *        from the frames it sends. Format and criteria: docs/acceptance.md.
 *
 * Only frames whose source address is the device under test's count; a TSC1
 * counts only if it is addressed to the engine (0x00). A stale observable
 * falls back to its default after 200 ms, as the simulated engine drops a
 * stale TSC1 and the simulated pump takes a stale command as zero: silence
 * counts as releasing.
 *
 * | Observable           | Value                                                            | Default |
 * |----------------------|------------------------------------------------------------------|---------|
 * | `tsc1_speed_control` | 1 if TSC1 asks for speed control (mode 01) with a valid speed, else 0 | 0  |
 * | `tsc1_speed_rpm`     | that requested speed while `tsc1_speed_control` is 1, else a gap | gap     |
 * | `pump_cmd_pct`       | OC_PUMP_CMD displacement; 0 if "not available"                    | 0       |
 * | `tsc1_override`      | 1 if TSC1 has an override mode other than 00 (speed, torque, limit), 0 if 00 | 0 |
 */

#ifndef OPEN_CRANK_ACCEPT_H
#define OPEN_CRANK_ACCEPT_H

#include <stddef.h>
#include <stdint.h>

#include <opencrank/oracle.h>
#include <opentick/can.h>

/** @brief Address of the device under test unless told otherwise: the reference controller's. */
#define OC_ACCEPT_DUT_DEFAULT 0x27U

/** @brief How long a decoded value holds, as the simulated engine and pump allow [ms]. */
#define OC_ACCEPT_STALE_MS 200U

typedef enum {
    OC_ACCEPT_TSC1_SPEED_CONTROL = 0,
    OC_ACCEPT_TSC1_SPEED_RPM,
    OC_ACCEPT_PUMP_CMD_PCT,
    OC_ACCEPT_TSC1_OVERRIDE,
    OC_ACCEPT_OBSERVABLE_COUNT
} OcAcceptObservable;

/** @brief The observables above, in that order. */
extern const OcOracleObservable OC_ACCEPT_OBSERVABLES[OC_ACCEPT_OBSERVABLE_COUNT];

/** @brief Decoder state: whose frames count. */
typedef struct {
    uint8_t dut; /**< source address of the device under test */
} OcAccept;

/** @brief Sets up @p a for the device under test at @p dut and fills @p table to use it. */
void oc_accept_init(OcAccept *a, uint8_t dut, OcOracleTable *table);

/** @brief The oracle's decoder (OcOracleDecodeFn); @p ctx is an OcAccept. */
size_t oc_accept_decode(void *ctx, const OtkCanFrame *f, OcOracleUpdate *out, size_t cap);

#endif /* OPEN_CRANK_ACCEPT_H */
