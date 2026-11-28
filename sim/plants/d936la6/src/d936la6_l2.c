/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file d936la6_l2.c
 * @brief Level 2 parameters of the d936la6 plant (injector and windows).
 *        Representative, not calibrated; provenance: docs/parameters.md.
 */

#include <opencrank/plants/d936la6.h>

const OcL2Params L2_D936LA6 = {
    .injector_dead_us = 200.0,
    .injector_full_us = 1600.0,
    .window_before_deg = 40.0,
    .window_after_deg = 20.0,
    .capture_delay_steps = 20u,
};
