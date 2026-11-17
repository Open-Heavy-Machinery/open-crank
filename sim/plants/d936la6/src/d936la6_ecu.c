/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file d936la6_ecu.c
 * @brief Calibration of the simulated engine ECU for the d936la6 plant.
 *        Provenance: docs/parameters.md.
 */

#include <opencrank/plants/d936la6.h>

const EngineEcuParams ENGINE_ECU_D936LA6 = {
    .low_idle_rpm = 750.0,
    .high_idle_rpm = 2100.0,
    .overspeed_trip_rpm = 2400.0,
    .fuel_enable_rpm = 100.0,
    .running_rpm = 500.0,
    .stall_rpm = 300.0,
    .start_fuel_pct = 30.0,
    .max_crank_s = 15.0,
    .kp_pct_per_rpm = 0.10,
    .ki_pct_per_rpm_s = 0.30,
    .droop_pct = 0.0,
    .request_timeout_ms = 200U,
    .source_address = 0x00U,
};
