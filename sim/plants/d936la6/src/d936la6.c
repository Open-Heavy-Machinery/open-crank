/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file d936la6.c
 * @brief Parameters of the d936la6 plant. Provenance: docs/parameters.md.
 */

#include <opencrank/plants/d936la6.h>

/* Full-load brake torque. No governor break-off region: the governed
 * ceiling belongs to the engine ECU, not to the physics. */
static const double D936_FL_RPM[] = { 600.0, 1000.0, 1350.0, 1600.0, 1900.0, 2100.0 };
static const double D936_FL_NM[]  = { 950.0, 1280.0, 1450.0, 1380.0, 1200.0, 1090.0 };

const PlantParams PLANT_D936LA6 = {
    .name = "d936la6 (D936L A6-class, representative)",

    .inertia_kgm2 = 1.85,
    .full_load_brake = { D936_FL_RPM, D936_FL_NM, sizeof(D936_FL_RPM) / sizeof(D936_FL_RPM[0]) },
    .reference_torque_nm = 1600.0,

    .friction_c0_nm = 45.0,
    .friction_c1_nm_per_rpm = 0.06,

    .boost_floor_frac = 0.45,
    .boost_tau_s = 1.2,

    .starter_stall_torque_nm = 300.0,
    .starter_no_load_rpm = 250.0,

    .coolant_initial_c = 20.0,
    .coolant_warm_c = 75.0,
    .coolant_gain_c_per_pct = 0.25,
    .coolant_tau_s = 100.0,

    .oil_min_rpm = 50.0,
    .oil_base_kpa = 120.0,
    .oil_kpa_per_rpm = 0.22,
    .oil_max_kpa = 550.0,
    .oil_tau_s = 0.5,
};
