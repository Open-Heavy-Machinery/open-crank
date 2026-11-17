/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file hydraulic_pump.c
 * @brief Load-sensing variable-displacement hydraulic pump. See hydraulic_pump.h.
 */

#include <opencrank/loads/hydraulic_pump.h>

#define OC_PI 3.14159265358979323846

const HydraulicPumpParams HYDRAULIC_PUMP_HOIST = {
    .displacement_max_cc_rev = 140.0,
    .gear_ratio = 1.0,
    .hm_efficiency = 0.90,
    .standby_pressure_bar = 20.0,
    .ls_margin_bar = 20.0,
    .relief_pressure_bar = 300.0,
    .standby_displacement_frac = 0.03,
    .active_threshold_pct = 1.0,
    .displacement_tau_s = 0.10,
    .pressure_tau_s = 0.25,
};

static double clamp(double v, double lo, double hi) {
    if (!(v >= lo)) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

static double lag(double state, double target, double dt, double tau) {
    if (tau <= 0.0 || dt >= tau) {
        return target;
    }
    return state + (target - state) * (dt / tau);
}

void hydraulic_pump_init(HydraulicPumpState *s, const HydraulicPumpParams *p) {
    s->displacement_frac = p->standby_displacement_frac;
    s->pressure_bar = 0.0;
    s->torque_nm = 0.0;
    s->flow_lpm = 0.0;
}

double hydraulic_pump_step(HydraulicPumpState *s, const HydraulicPumpParams *p, double displacement_cmd_pct,
                           double load_pressure_bar, double engine_rpm, double dt) {
    const double cmd_pct = clamp(displacement_cmd_pct, 0.0, 100.0);
    const int active = cmd_pct > p->active_threshold_pct;
    const double pump_rpm = engine_rpm * p->gear_ratio;
    const int turning = pump_rpm > 1.0;

    /* Targets: a function is active -> stroke to the command and hold load
     * pressure plus margin; otherwise destroke and hold standby pressure. */
    double disp_target = p->standby_displacement_frac;
    double pressure_target = p->standby_pressure_bar;
    if (active) {
        disp_target = clamp(cmd_pct / 100.0, p->standby_displacement_frac, 1.0);
        pressure_target = clamp(load_pressure_bar + p->ls_margin_bar, p->standby_pressure_bar,
                                p->relief_pressure_bar);
    }
    if (!turning) {
        pressure_target = 0.0; /* a stopped pump holds no pressure */
    }

    s->displacement_frac = lag(s->displacement_frac, disp_target, dt, p->displacement_tau_s);
    s->pressure_bar = lag(s->pressure_bar, pressure_target, dt, p->pressure_tau_s);

    if (!turning) {
        s->torque_nm = 0.0;
        s->flow_lpm = 0.0;
        return 0.0;
    }

    const double vg_m3 = p->displacement_max_cc_rev * 1e-6 * s->displacement_frac;
    const double pump_torque = vg_m3 * (s->pressure_bar * 1e5) / (2.0 * OC_PI * p->hm_efficiency);
    s->torque_nm = pump_torque * p->gear_ratio;
    s->flow_lpm = p->displacement_max_cc_rev * s->displacement_frac * pump_rpm / 1000.0;
    return s->torque_nm;
}
