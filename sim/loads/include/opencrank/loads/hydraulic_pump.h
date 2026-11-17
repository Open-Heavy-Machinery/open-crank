/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hydraulic_pump.h
 * @brief Load-sensing variable-displacement hydraulic pump driven by the engine.
 *
 * The machine controller commands displacement (flow); the load on the hook
 * sets pressure. Absorbed torque at the crankshaft:
 *
 *   T = ratio * Vg_max * disp * p / (2 * pi * eta_hm)
 *
 * With no function active the pump destrokes to a small standby displacement
 * and holds standby pressure, so its idle torque is small.
 */

#ifndef OPEN_CRANK_LOADS_HYDRAULIC_PUMP_H
#define OPEN_CRANK_LOADS_HYDRAULIC_PUMP_H

/** @brief Static pump parameters. Provenance: docs/parameters.md. */
typedef struct {
    double displacement_max_cc_rev;  /**< maximum geometric displacement */
    double gear_ratio;               /**< pump speed / engine speed */
    double hm_efficiency;            /**< hydro-mechanical efficiency, 0..1 */
    double standby_pressure_bar;     /**< pressure held with no function active */
    double ls_margin_bar;            /**< load-sensing margin above load pressure */
    double relief_pressure_bar;      /**< main relief setting */
    double standby_displacement_frac;/**< displacement held with no function active */
    double active_threshold_pct;     /**< displacement command above which a function is active */
    double displacement_tau_s;       /**< swash-plate response */
    double pressure_tau_s;           /**< pressure build-up (fluid and hose compliance) */
} HydraulicPumpParams;

/** @brief Pump state and telemetry. */
typedef struct {
    double displacement_frac;  /**< actual swash-plate position, 0..1 */
    double pressure_bar;       /**< actual outlet pressure */
    double torque_nm;          /**< torque absorbed at the crankshaft */
    double flow_lpm;           /**< delivered flow */
} HydraulicPumpState;

/** @brief Hoist pump of the reference crane. */
extern const HydraulicPumpParams HYDRAULIC_PUMP_HOIST;

void hydraulic_pump_init(HydraulicPumpState *s, const HydraulicPumpParams *p);

/**
 * @brief Advances the pump by @p dt seconds.
 * @param displacement_cmd_pct displacement command from the machine controller, 0..100 %
 * @param load_pressure_bar    pressure required by the load (from the scenario)
 * @param engine_rpm           crankshaft speed
 * @return torque absorbed at the crankshaft [Nm]
 */
double hydraulic_pump_step(HydraulicPumpState *s, const HydraulicPumpParams *p, double displacement_cmd_pct,
                           double load_pressure_bar, double engine_rpm, double dt);

#endif /* OPEN_CRANK_LOADS_HYDRAULIC_PUMP_H */
