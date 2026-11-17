/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plant.c
 * @brief Mean-value engine plant: physics only. See plant.h for the model.
 */

#include <opencrank/plant.h>

#include <string.h>

#define OC_PI 3.14159265358979323846
#define RPM_TO_RAD_S (2.0 * OC_PI / 60.0)

static double clamp(double v, double lo, double hi) {
    if (!(v >= lo)) { /* also catches NaN */
        return lo;
    }
    return (v > hi) ? hi : v;
}

static double min2(double a, double b) {
    return (a < b) ? a : b;
}

double plant_table_lookup(const PlantTable *t, double x) {
    if (t->n == 0U) {
        return 0.0;
    }
    if (!(x > t->x[0])) {
        return t->y[0];
    }
    if (x >= t->x[t->n - 1U]) {
        return t->y[t->n - 1U];
    }
    for (size_t i = 1U; i < t->n; i++) {
        if (x <= t->x[i]) {
            double f = (x - t->x[i - 1U]) / (t->x[i] - t->x[i - 1U]);
            return t->y[i - 1U] + f * (t->y[i] - t->y[i - 1U]);
        }
    }
    return t->y[t->n - 1U];
}

double plant_full_load_brake_nm(const PlantParams *p, double rpm) {
    return plant_table_lookup(&p->full_load_brake, rpm);
}

double plant_friction_nm(const PlantParams *p, double rpm) {
    return (rpm > 0.0) ? (p->friction_c0_nm + p->friction_c1_nm_per_rpm * rpm) : 0.0;
}

void plant_init(PlantState *s, const PlantParams *p) {
    memset(s, 0, sizeof(*s));
    s->coolant_temp_c = p->coolant_initial_c;
}

void plant_step(PlantState *s, const PlantParams *p, const PlantInputs *in, double dt) {
    const double rpm = s->rpm;
    const double fuel_pct = clamp(in->fuel_pct, 0.0, 100.0);

    /* 1. Combustion, limited by the air the turbocharger has delivered so far.
     *    No rotation means no compression stroke and no combustion torque. */
    const double air_frac = p->boost_floor_frac + (1.0 - p->boost_floor_frac) * s->boost_frac;
    const double burnt_frac = min2(fuel_pct / 100.0, air_frac);
    double indicated = 0.0;
    if (rpm > 0.0) {
        const double full_load_indicated =
            plant_full_load_brake_nm(p, rpm) + p->friction_c0_nm + p->friction_c1_nm_per_rpm * rpm;
        indicated = burnt_frac * full_load_indicated;
    }

    /* 2. Starter: DC-motor torque line, freewheels above its no-load speed. */
    double starter = 0.0;
    if (in->starter_engaged && p->starter_no_load_rpm > 0.0 && rpm < p->starter_no_load_rpm) {
        starter = p->starter_stall_torque_nm * (1.0 - rpm / p->starter_no_load_rpm);
    }

    /* 3. Friction. At standstill it is static: it holds the shaft up to c0. */
    const double drive = indicated + starter - in->load_torque_nm;
    double friction;
    if (rpm > 0.0) {
        friction = plant_friction_nm(p, rpm);
    } else {
        friction = clamp(drive, 0.0, p->friction_c0_nm);
    }

    /* 4. Crankshaft dynamics; the shaft does not turn backwards. */
    const double net = drive - friction;
    double omega = rpm * RPM_TO_RAD_S + (net / p->inertia_kgm2) * dt;
    if (omega < 0.0) {
        omega = 0.0;
    }
    s->rpm = omega / RPM_TO_RAD_S;

    /* 5. Air path follows the fuel actually burnt (exhaust energy spools the turbo). */
    if (p->boost_tau_s > 0.0) {
        s->boost_frac += (burnt_frac - s->boost_frac) * (dt / p->boost_tau_s);
    } else {
        s->boost_frac = burnt_frac;
    }
    s->boost_frac = clamp(s->boost_frac, 0.0, 1.0);

    /* 6. Placeholder thermal and lubrication states (first-order lags). */
    const bool turning = rpm > p->oil_min_rpm;
    const double coolant_target = turning ? (p->coolant_warm_c + p->coolant_gain_c_per_pct * fuel_pct)
                                          : p->coolant_initial_c;
    if (p->coolant_tau_s > 0.0) {
        s->coolant_temp_c += (coolant_target - s->coolant_temp_c) * (dt / p->coolant_tau_s);
    }
    const double oil_target = turning ? min2(p->oil_base_kpa + p->oil_kpa_per_rpm * rpm, p->oil_max_kpa) : 0.0;
    if (p->oil_tau_s > 0.0) {
        s->oil_pressure_kpa += (oil_target - s->oil_pressure_kpa) * min2(dt / p->oil_tau_s, 1.0);
    }

    /* 7. Telemetry. */
    s->fuel_pct = fuel_pct;
    s->indicated_torque_nm = indicated;
    s->friction_torque_nm = friction;
    s->starter_torque_nm = starter;
    s->load_torque_nm = in->load_torque_nm;
    s->brake_torque_nm = indicated - friction;
    s->net_torque_nm = net;
}
