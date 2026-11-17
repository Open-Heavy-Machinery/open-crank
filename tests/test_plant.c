/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_plant.c
 * @brief Plant physics: no self-start, cranking, spin-down, clamping,
 *        full-load torque, air-path lag, determinism.
 */

#include <string.h>

#include "check.h"
#include <opencrank/plants/d936la6.h>

static const PlantParams *const P = &PLANT_D936LA6;
static const double DT = 0.010;

static void run(PlantState *s, const PlantInputs *in, double seconds) {
    const int steps = (int)(seconds / DT + 0.5);
    for (int i = 0; i < steps; i++) {
        plant_step(s, P, in, DT);
    }
}

/* A plant must not start itself: that is the ECU's or the starter's job. */
static void test_no_self_start(void) {
    PlantState s;
    plant_init(&s, P);
    PlantInputs in = { .fuel_pct = 0.0, .load_torque_nm = 0.0, .starter_engaged = false };
    run(&s, &in, 5.0);
    CHECK(s.rpm == 0.0);

    /* Even full fuel produces no torque on a stationary shaft. */
    in.fuel_pct = 100.0;
    run(&s, &in, 5.0);
    CHECK(s.rpm == 0.0);
    CHECK(s.indicated_torque_nm == 0.0);
}

/* The starter alone turns the engine at cranking speed, below its no-load speed. */
static void test_starter_cranks(void) {
    PlantState s;
    plant_init(&s, P);
    PlantInputs in = { .fuel_pct = 0.0, .load_torque_nm = 0.0, .starter_engaged = true };
    run(&s, &in, 3.0);
    CHECK(s.rpm > 150.0);
    CHECK(s.rpm < 250.0);
    CHECK(s.rpm < P->starter_no_load_rpm);
}

/* With no fuel the engine slows down monotonically and stops. */
static void test_spin_down(void) {
    PlantState s;
    plant_init(&s, P);
    s.rpm = 1500.0;
    PlantInputs in = { .fuel_pct = 0.0, .load_torque_nm = 0.0, .starter_engaged = false };
    double prev = s.rpm;
    int monotonic = 1;
    for (int i = 0; i < 1000; i++) {
        plant_step(&s, P, &in, DT);
        if (s.rpm > prev) {
            monotonic = 0;
        }
        prev = s.rpm;
    }
    CHECK(monotonic);
    CHECK(s.rpm == 0.0);
}

/* Fuel outside 0..100 % is clamped by the plant. */
static void test_fuel_clamped(void) {
    PlantState a;
    PlantState b;
    plant_init(&a, P);
    plant_init(&b, P);
    a.rpm = b.rpm = 1200.0;
    a.boost_frac = b.boost_frac = 1.0;
    PlantInputs over = { .fuel_pct = 150.0, .load_torque_nm = 0.0, .starter_engaged = false };
    PlantInputs full = { .fuel_pct = 100.0, .load_torque_nm = 0.0, .starter_engaged = false };
    plant_step(&a, P, &over, DT);
    plant_step(&b, P, &full, DT);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);

    PlantInputs under = { .fuel_pct = -20.0, .load_torque_nm = 0.0, .starter_engaged = false };
    plant_step(&a, P, &under, DT);
    CHECK(a.indicated_torque_nm == 0.0);
}

/* At full fuel with the air path settled, brake torque equals the datasheet
 * table. This catches friction being subtracted twice. */
static void test_full_load_brake_matches_table(void) {
    const double speeds[] = { 600.0, 1000.0, 1350.0, 1600.0, 1900.0, 2100.0 };
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        PlantState s;
        plant_init(&s, P);
        s.rpm = speeds[i];
        s.boost_frac = 1.0;
        PlantInputs in = { .fuel_pct = 100.0, .load_torque_nm = 0.0, .starter_engaged = false };
        plant_step(&s, P, &in, 1e-6);
        CHECK_NEAR(s.brake_torque_nm, plant_full_load_brake_nm(P, speeds[i]), 1e-6);
    }
    /* Rated point of the class: about 240 kW at 1900 rpm. */
    const double rated_kw = plant_full_load_brake_nm(P, 1900.0) * 1900.0 * 2.0 * 3.14159265358979 / 60.0 / 1000.0;
    CHECK_NEAR(rated_kw, 240.0, 5.0);
}

/* A sudden fuel step is limited by boost; torque builds with the air path. */
static void test_air_path_lag(void) {
    PlantState s;
    plant_init(&s, P);
    s.rpm = 1500.0;
    PlantInputs in = { .fuel_pct = 100.0, .load_torque_nm = 0.0, .starter_engaged = false };
    const double full = plant_full_load_brake_nm(P, 1500.0) + plant_friction_nm(P, 1500.0);

    plant_step(&s, P, &in, DT);
    CHECK_NEAR(s.indicated_torque_nm, P->boost_floor_frac * full, 1e-6);

    /* The turbo spools on burnt fuel, so from zero boost at full fuel the
     * effective time constant is boost_tau / boost_floor. */
    const double effective_tau = P->boost_tau_s / P->boost_floor_frac;
    plant_step(&s, P, &in, DT);
    CHECK(s.indicated_torque_nm < 0.5 * full);
    for (int i = 0; i < (int)(6.0 * effective_tau / DT); i++) {
        s.rpm = 1500.0; /* hold speed to look at the air path alone */
        plant_step(&s, P, &in, DT);
    }
    CHECK(s.indicated_torque_nm > 0.99 * full);
}

/* Static friction holds the shaft against small torques; larger ones turn it. */
static void test_static_friction_and_overrun(void) {
    PlantState s;
    plant_init(&s, P);
    PlantInputs in = { .fuel_pct = 0.0, .load_torque_nm = -0.5 * P->friction_c0_nm, .starter_engaged = false };
    run(&s, &in, 1.0);
    CHECK(s.rpm == 0.0);

    in.load_torque_nm = -200.0; /* an overrunning load drives the engine */
    run(&s, &in, 1.0);
    CHECK(s.rpm > 100.0);
}

/* Same inputs, same outputs, bit for bit. */
static void test_deterministic(void) {
    PlantState a;
    PlantState b;
    plant_init(&a, P);
    plant_init(&b, P);
    for (int i = 0; i < 3000; i++) {
        PlantInputs in = {
            .fuel_pct = (double)(i % 97),
            .load_torque_nm = (double)(i % 13) * 20.0,
            .starter_engaged = i < 300,
        };
        plant_step(&a, P, &in, DT);
        plant_step(&b, P, &in, DT);
    }
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(a.rpm > 0.0);
}

int main(void) {
    RUN_TEST(test_no_self_start);
    RUN_TEST(test_starter_cranks);
    RUN_TEST(test_spin_down);
    RUN_TEST(test_fuel_clamped);
    RUN_TEST(test_full_load_brake_matches_table);
    RUN_TEST(test_air_path_lag);
    RUN_TEST(test_static_friction_and_overrun);
    RUN_TEST(test_deterministic);
    return check_report();
}
