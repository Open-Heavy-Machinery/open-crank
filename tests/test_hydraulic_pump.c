/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_hydraulic_pump.c
 * @brief Hydraulic pump load: torque formula, standby, relief, lags, standstill.
 */

#include "check.h"
#include <opencrank/loads/hydraulic_pump.h>

static const HydraulicPumpParams *const P = &HYDRAULIC_PUMP_HOIST;
static const double DT = 0.010;
static const double PI = 3.14159265358979323846;

static double settle(HydraulicPumpState *s, double cmd_pct, double load_bar, double rpm) {
    double t = 0.0;
    for (int i = 0; i < 500; i++) {
        t = hydraulic_pump_step(s, P, cmd_pct, load_bar, rpm, DT);
    }
    return t;
}

static void test_torque_formula(void) {
    HydraulicPumpState s;
    hydraulic_pump_init(&s, P);
    const double t = settle(&s, 100.0, 230.0, 1500.0);
    const double expected = P->displacement_max_cc_rev * 1e-6 * (230.0 + P->ls_margin_bar) * 1e5 /
                            (2.0 * PI * P->hm_efficiency) * P->gear_ratio;
    CHECK_NEAR(s.pressure_bar, 230.0 + P->ls_margin_bar, 1e-6);
    CHECK_NEAR(t, expected, 1e-6);
    CHECK_NEAR(s.flow_lpm, P->displacement_max_cc_rev * 1500.0 * P->gear_ratio / 1000.0, 1e-6);
}

static void test_standby_is_small(void) {
    HydraulicPumpState s;
    hydraulic_pump_init(&s, P);
    const double t = settle(&s, 0.0, 250.0, 1500.0);
    CHECK_NEAR(s.pressure_bar, P->standby_pressure_bar, 1e-6);
    CHECK_NEAR(s.displacement_frac, P->standby_displacement_frac, 1e-9);
    CHECK(t < 5.0);
}

static void test_relief_limits_pressure(void) {
    HydraulicPumpState s;
    hydraulic_pump_init(&s, P);
    (void)settle(&s, 100.0, 1000.0, 1500.0);
    CHECK_NEAR(s.pressure_bar, P->relief_pressure_bar, 1e-6);
}

static void test_load_builds_with_lag(void) {
    HydraulicPumpState s;
    hydraulic_pump_init(&s, P);
    (void)settle(&s, 0.0, 0.0, 1500.0);
    const double before = s.torque_nm;
    const double first = hydraulic_pump_step(&s, P, 100.0, 250.0, 1500.0, DT);
    CHECK(first > before);
    CHECK(first < 0.2 * settle(&s, 100.0, 250.0, 1500.0));
}

static void test_no_torque_at_standstill(void) {
    HydraulicPumpState s;
    hydraulic_pump_init(&s, P);
    CHECK(settle(&s, 100.0, 250.0, 0.0) == 0.0);
    CHECK(s.flow_lpm == 0.0);
}

int main(void) {
    RUN_TEST(test_torque_formula);
    RUN_TEST(test_standby_is_small);
    RUN_TEST(test_relief_limits_pressure);
    RUN_TEST(test_load_builds_with_lag);
    RUN_TEST(test_no_torque_at_standstill);
    return check_report();
}
