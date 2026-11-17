/**
 * @file plant.c
 * @brief Implementation of the core internal combustion engine plant simulation dynamics.
 */

#include <opencrank/plant.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void plant_init(PlantState *state, const PlantParams *params, double initial_rpm) {
    (void)params; /* Unused parameter in base initialization */
    memset(state, 0, sizeof(*state));
    
    state->rpm = initial_rpm;
    state->coolant_temp_c = 40.0; /* Baseline warm-up coolant temperature (°C) */
    
    /* Establish nominal oil pressure if the engine starts already spinning */
    state->oil_pressure_kpa = (initial_rpm > 100.0) ? 350.0 : 0.0;
}

void plant_step(PlantState *state, const PlantParams *params, const PlantInputs *inputs, double dt) {
    /* 1. Combustion torque via plant-specific polymorphic hook */
    double indicated_tq = 0.0;
    if (params->calc_indicated_torque != NULL) {
        indicated_tq = params->calc_indicated_torque(state, inputs->throttle_pct, params->user_ctx);
    }

    /* 2. Starter motor contribution (disengages automatically above 400 RPM) */
    if (inputs->starter_active && state->rpm < 400.0) {
        indicated_tq += params->starter_torque_nm;
    }

    /* 3. Internal mechanical friction and pumping losses via plant-specific hook */
    double friction_tq = 0.0;
    if (params->calc_friction_torque != NULL) {
        friction_tq = params->calc_friction_torque(state, params->user_ctx);
    }

    /* 4. Rotational dynamics: J * alpha = sum(Torque) */
    double net_tq = indicated_tq - friction_tq - inputs->load_torque_nm;
    double alpha_rad_s2 = net_tq / params->inertia_kgm2;

    /* Convert RPM to rad/s, integrate angular acceleration, and clamp against reverse rotation */
    double omega_rad_s = (state->rpm * 2.0 * M_PI) / 60.0;
    omega_rad_s += alpha_rad_s2 * dt;
    if (omega_rad_s < 0.0) {
        omega_rad_s = 0.0;
    }

    /* Update crankshaft speed, wrapped angular position [0, 2*PI), and torque telemetry */
    state->rpm = (omega_rad_s * 60.0) / (2.0 * M_PI);
    state->crank_angle_rad = fmod(state->crank_angle_rad + (omega_rad_s * dt), 2.0 * M_PI);
    state->indicated_tq_nm = indicated_tq;
    state->friction_tq_nm = friction_tq;
    state->net_torque_nm = net_tq;

    /* 5. First-order lag models for thermal dynamics and oil pressure */
    double target_temp = 75.0 + (inputs->throttle_pct * 0.25);
    state->coolant_temp_c += (target_temp - state->coolant_temp_c) * 0.01 * dt;

    /* Oil pressure scales linearly with RPM above 50 RPM, capped at 550 kPa */
    double target_oil_kpa = (state->rpm > 50.0) ? (120.0 + (state->rpm * 0.22)) : 0.0;
    if (target_oil_kpa > 550.0) {
        target_oil_kpa = 550.0;
    }
    state->oil_pressure_kpa += (target_oil_kpa - state->oil_pressure_kpa) * 2.0 * dt;
}
