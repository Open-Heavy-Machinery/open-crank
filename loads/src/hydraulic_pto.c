/**
 * @file hydraulic_pto.c
 * @brief Implementation of the hydraulic Power Take-Off (PTO) pump load dynamics and torque calculation.
 */

#include <opencrank/loads/hydraulic_pto.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void hydraulic_pto_init(HydraulicPto *pto) {
    pto->displacement_cc_rev = 140.0; /* Typical mobile crane hydraulic pump displacement (cc/rev) */
    pto->mech_efficiency = 0.90;      /* Nominal hydromechanical efficiency factor */
    pto->base_pressure_bar = 20.0;    /* Unloaded standby pressure (bar) */
    pto->max_pressure_bar = 300.0;    /* Maximum system working pressure (bar) */
    pto->current_pressure_bar = 20.0; /* Initial line pressure at standby */
}

double hydraulic_pto_step(HydraulicPto *pto, double valve_demand_0_to_1, double engine_rpm, double dt) {
    /* Return zero resistive torque if the pump shaft is effectively stationary */
    if (engine_rpm < 10.0) {
        return 0.0;
    }

    /* Clamp normalized valve/joystick demand command to [0.0, 1.0] */
    if (valve_demand_0_to_1 < 0.0) {
        valve_demand_0_to_1 = 0.0;
    }
    if (valve_demand_0_to_1 > 1.0) {
        valve_demand_0_to_1 = 1.0;
    }

    /* 1. Compute target system pressure linearly from proportional hoist/boom valve demand */
    double target_bar = pto->base_pressure_bar + 
                        valve_demand_0_to_1 * (pto->max_pressure_bar - pto->base_pressure_bar);

    /* 2. First-order hydraulic pressure ramp (fluid inertia and hose compliance, tau ~ 0.25 s) */
    pto->current_pressure_bar += (target_bar - pto->current_pressure_bar) * 4.0 * dt;

    /* 3. Convert hydraulic parameters to SI units: cc/rev -> m^3/rev, bar -> Pa */
    double vg_m3_rev = pto->displacement_cc_rev * 1e-6;
    double delta_p_pa = pto->current_pressure_bar * 1e5;

    /* 4. Compute resistive shaft torque: T = (Vg * dP) / (2 * pi * eta_mech) */
    return (vg_m3_rev * delta_p_pa) / (2.0 * M_PI * pto->mech_efficiency);
}
