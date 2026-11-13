/**
 * @file d936la6.c
 * @brief Physics model implementation, torque lookup curves, and parameters for the Liebherr D936L A6 diesel engine.
 */

#include <opencrank/plants/d936la6.h>
#include <stddef.h>

/** @brief Engine speed breakpoints (RPM) for the full-load torque lookup table. */
static const double D936_RPM_AXIS[] = {   0.0,  600.0, 1000.0, 1350.0, 1600.0, 1900.0, 2200.0 };

/** @brief Maximum gross indicated torque values (Nm) corresponding to D936_RPM_AXIS. */
static const double D936_MAX_TQ[]   = { 400.0,  950.0, 1280.0, 1450.0, 1380.0, 1200.0,  300.0 };

/** @brief Number of elements in the D936L A6 torque lookup table. */
#define D936_LUT_SIZE (sizeof(D936_RPM_AXIS) / sizeof(D936_RPM_AXIS[0]))

/**
 * @brief Performs piecewise 1D linear interpolation with boundary clamping.
 * 
 * @param x Monotonically increasing array of independent axis breakpoints.
 * @param y Array of dependent values corresponding to each breakpoint in x.
 * @param n Number of elements in the lookup table arrays.
 * @param xi Query point on the independent axis.
 * @return Interpolated value, clamped to y[0] or y[n-1] if xi lies outside the table range.
 */
static double interp1d(const double *x, const double *y, size_t n, double xi) {
    if (xi <= x[0]) {
        return y[0];
    }
    if (xi >= x[n - 1]) {
        return y[n - 1];
    }

    for (size_t i = 0; i < n - 1; i++) {
        if (xi >= x[i] && xi <= x[i + 1]) {
            double t = (xi - x[i]) / (x[i + 1] - x[i]);
            return y[i] + t * (y[i + 1] - y[i]);
        }
    }
    return 0.0;
}

/**
 * @brief Computes gross indicated combustion torque for the D936L A6 engine.
 *        Includes a proportional low-idle speed governor that prevents stalling below 750 RPM.
 * 
 * @param state Pointer to the current engine state.
 * @param throttle_pct External throttle/fuel demand command [0.0, 100.0].
 * @param user_ctx Unused opaque context pointer.
 * @return Gross indicated combustion torque in Newton-meters (Nm).
 */
static double d936_calc_torque(const PlantState *state, double throttle_pct, void *user_ctx) {
    (void)user_ctx;
    double max_tq = interp1d(D936_RPM_AXIS, D936_MAX_TQ, D936_LUT_SIZE, state->rpm);

    /* Clamp incoming throttle command to valid percentage bounds */
    double effective_throttle = throttle_pct;
    if (effective_throttle < 0.0) {
        effective_throttle = 0.0;
    }
    if (effective_throttle > 100.0) {
        effective_throttle = 100.0;
    }

    /* Minimum idle governor: applies proportional fuel compensation to hold ~750 RPM at zero throttle */
    if (state->rpm < 750.0 && effective_throttle < 10.0) {
        effective_throttle = (750.0 - state->rpm) * 0.2;
        if (effective_throttle > 25.0) {
            effective_throttle = 25.0;
        }
    }

    return max_tq * (effective_throttle / 100.0);
}

/**
 * @brief Computes resistive mechanical friction and pumping losses for the D936L A6 engine.
 * 
 * @param state Pointer to the current engine state.
 * @param user_ctx Unused opaque context pointer.
 * @return Resistive friction torque in Newton-meters (Nm).
 */
static double d936_calc_friction(const PlantState *state, void *user_ctx) {
    (void)user_ctx;
    if (state->rpm <= 0.1) {
        return 0.0; /* No dynamic friction when crankshaft is stationary */
    }
    /* Coulomb breakaway friction (45 Nm) plus speed-dependent viscous/pumping losses */
    return 45.0 + (0.06 * state->rpm);
}

/**
 * @brief Exported static plant configuration and callbacks for the Liebherr D936L A6.
 */
const PlantParams PLANT_D936LA6 = {
    .name = "Liebherr D936L A6",
    .inertia_kgm2 = 1.85,
    .idle_rpm = 750.0,
    .max_rpm = 2100.0,
    .starter_torque_nm = 250.0,
    .calc_indicated_torque = d936_calc_torque,
    .calc_friction_torque = d936_calc_friction,
    .user_ctx = NULL
};
