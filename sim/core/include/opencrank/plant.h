/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plant.h
 * @brief Mean-value engine plant: physics only.
 *
 * The plant answers back; it never decides. It has no governor, no starter
 * logic, no clock, no I/O and no global state. Fuel and starter engagement
 * come from outside (the simulated engine ECU in Level 1, the device under
 * test in Level 2). Same inputs give the same outputs, bit for bit.
 *
 * Model (see ARCHITECTURE.md, section 4.1):
 *
 *   J * domega/dt = T_ind - T_fric - T_load + T_starter
 *   T_ind         = min(fuel, air) * (T_fl_brake(n) + T_fric(n))
 *   air           = boost_floor + (1 - boost_floor) * boost
 *   dboost/dt     = (min(fuel, air) - boost) / boost_tau
 *
 * T_fl_brake is the full-load brake torque curve as a datasheet gives it,
 * so at full fuel and settled boost the brake torque equals the table.
 *
 * The turbocharger spools on burnt fuel, which itself is limited by boost.
 * From zero boost at full fuel this gives boost(t) = 1 - exp(-t * floor / tau):
 * the effective load-acceptance time constant is boost_tau / boost_floor.
 */

#ifndef OPEN_CRANK_PLANT_H
#define OPEN_CRANK_PLANT_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Piecewise-linear table, clamped at both ends. */
typedef struct {
    const double *x;  /**< strictly increasing breakpoints */
    const double *y;  /**< values at the breakpoints */
    size_t n;         /**< number of breakpoints (>= 1) */
} PlantTable;

/** @brief Static parameters of one engine. Every value needs provenance in docs/parameters.md. */
typedef struct {
    const char *name;

    double inertia_kgm2;          /**< rotating inertia seen by the crankshaft */
    PlantTable full_load_brake;   /**< full-load brake torque [Nm] vs speed [rpm], no governor region */
    double reference_torque_nm;   /**< J1939 engine reference torque (SPN 544) for percent conversions */

    double friction_c0_nm;        /**< friction and pumping torque: c0 + c1 * rpm */
    double friction_c1_nm_per_rpm;

    double boost_floor_frac;      /**< fraction of full-load torque available with no boost */
    double boost_tau_s;           /**< air-path (turbocharger) time constant */

    double starter_stall_torque_nm; /**< starter torque at 0 rpm, referred to the crankshaft */
    double starter_no_load_rpm;     /**< crankshaft speed at which starter torque falls to zero */

    double coolant_initial_c;     /**< coolant temperature at plant_init (ambient) */
    double coolant_warm_c;        /**< settled coolant temperature at zero fuel while running */
    double coolant_gain_c_per_pct;/**< extra settled temperature per percent fuel */
    double coolant_tau_s;         /**< coolant warm-up time constant */

    double oil_min_rpm;           /**< below this speed the oil pump delivers no pressure */
    double oil_base_kpa;          /**< oil pressure intercept */
    double oil_kpa_per_rpm;       /**< oil pressure slope */
    double oil_max_kpa;           /**< pressure relief setting */
    double oil_tau_s;             /**< oil pressure time constant */
} PlantParams;

/** @brief Inputs applied during one step. */
typedef struct {
    double fuel_pct;         /**< fuel demand, percent of full-load fuelling at the current speed; clamped to 0..100 */
    double load_torque_nm;   /**< resistive torque from the load; negative values drive the engine */
    bool starter_engaged;    /**< starter pinion engaged (decided outside the plant) */
} PlantInputs;

/** @brief Plant state and the telemetry of the last step. */
typedef struct {
    /* states */
    double rpm;              /**< crankshaft speed, >= 0 */
    double boost_frac;       /**< air-path state, 0..1 */
    double coolant_temp_c;
    double oil_pressure_kpa;

    /* telemetry from the last step */
    double fuel_pct;            /**< fuel demand after clamping */
    double indicated_torque_nm; /**< combustion torque actually produced */
    double friction_torque_nm;
    double starter_torque_nm;
    double load_torque_nm;
    double brake_torque_nm;     /**< indicated - friction (flywheel torque) */
    double net_torque_nm;       /**< accelerating torque */
} PlantState;

/** @brief Linear interpolation in @p t, clamped to the end values. */
double plant_table_lookup(const PlantTable *t, double x);

/** @brief Full-load brake torque at @p rpm. */
double plant_full_load_brake_nm(const PlantParams *p, double rpm);

/** @brief Friction and pumping torque at @p rpm (zero at standstill). */
double plant_friction_nm(const PlantParams *p, double rpm);

/** @brief Puts the engine at rest: 0 rpm, no boost, coolant at its initial temperature. */
void plant_init(PlantState *s, const PlantParams *p);

/** @brief Advances the plant by @p dt seconds (explicit Euler, fixed step). */
void plant_step(PlantState *s, const PlantParams *p, const PlantInputs *in, double dt);

#endif /* OPEN_CRANK_PLANT_H */
