/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_engine_ecu.c
 * @brief Simulated engine ECU, alone and closed-loop with the d936la6 plant.
 */

#include "check.h"
#include <opencrank/plants/d936la6.h>

static const PlantParams *const PP = &PLANT_D936LA6;
static const EngineEcuParams *const EP = &ENGINE_ECU_D936LA6;
static const uint32_t DT_MS = 10U;

#define MC_ADDR 0x27U

/* ECU plus plant, stepped together. */
typedef struct {
    EngineEcu ecu;
    PlantState plant;
    EngineEcuInputs in;
    double load_nm;
    double max_rpm;
} Rig;

static void rig_init(Rig *r) {
    engine_ecu_init(&r->ecu, EP);
    plant_init(&r->plant, PP);
    r->in.key_on = true;
    r->in.start = false;
    r->in.rpm = 0.0;
    r->load_nm = 0.0;
    r->max_rpm = 0.0;
}

static void rig_step(Rig *r) {
    EngineEcuOutputs out;
    r->in.rpm = r->plant.rpm;
    engine_ecu_step(&r->ecu, EP, &r->in, DT_MS, &out);
    PlantInputs pin = { .fuel_pct = out.fuel_pct, .load_torque_nm = r->load_nm, .starter_engaged = out.starter_engaged };
    plant_step(&r->plant, PP, &pin, (double)DT_MS / 1000.0);
    if (r->plant.rpm > r->max_rpm) {
        r->max_rpm = r->plant.rpm;
    }
}

static void rig_run(Rig *r, double seconds) {
    const int steps = (int)(seconds * 1000.0 / (double)DT_MS + 0.5);
    for (int i = 0; i < steps; i++) {
        rig_step(r);
    }
}

/* Runs while sending a speed request every 10 ms, as a machine controller would. */
static void rig_run_requesting(Rig *r, double seconds, double rpm) {
    const EngineEcuRequest req = { .mode = ECU_OVERRIDE_SPEED, .speed_valid = true, .speed_rpm = rpm, .source_address = MC_ADDR };
    const int steps = (int)(seconds * 1000.0 / (double)DT_MS + 0.5);
    for (int i = 0; i < steps; i++) {
        engine_ecu_request(&r->ecu, &req);
        rig_step(r);
    }
}

static void rig_start(Rig *r) {
    r->in.start = true;
    rig_run(r, 3.0);
    r->in.start = false;
    rig_run(r, 7.0);
}

static void test_key_off_means_no_fuel(void) {
    EngineEcu e;
    engine_ecu_init(&e, EP);
    EngineEcuInputs in = { .key_on = false, .start = true, .rpm = 1000.0 };
    EngineEcuOutputs out;
    engine_ecu_step(&e, EP, &in, DT_MS, &out);
    CHECK(out.fuel_pct == 0.0);
    CHECK(!out.starter_engaged);
    CHECK(e.state == ECU_STATE_OFF);
}

static void test_no_fuel_below_enable_speed(void) {
    EngineEcu e;
    engine_ecu_init(&e, EP);
    EngineEcuInputs in = { .key_on = true, .start = true, .rpm = 0.5 * EP->fuel_enable_rpm };
    EngineEcuOutputs out;
    for (int i = 0; i < 3; i++) {
        engine_ecu_step(&e, EP, &in, DT_MS, &out);
    }
    CHECK(e.state == ECU_STATE_CRANKING);
    CHECK(out.starter_engaged);
    CHECK(out.fuel_pct == 0.0);

    in.rpm = 1.5 * EP->fuel_enable_rpm;
    engine_ecu_step(&e, EP, &in, DT_MS, &out);
    CHECK(out.fuel_pct == EP->start_fuel_pct);
}

static void test_start_and_idle(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    CHECK(r.ecu.state == ECU_STATE_RUNNING);
    CHECK(!r.ecu.starter_engaged);
    CHECK(r.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    CHECK(r.ecu.controlling_address == EP->source_address);
    CHECK_NEAR(r.plant.rpm, EP->low_idle_rpm, 5.0);
    CHECK(r.max_rpm < EP->low_idle_rpm + 250.0);
}

static void test_releasing_start_early_aborts(void) {
    Rig r;
    rig_init(&r);
    r.in.start = true;
    rig_run(&r, 0.05);
    r.in.start = false;
    rig_run(&r, 3.0);
    CHECK(r.ecu.state == ECU_STATE_STOPPED);
    CHECK(!r.ecu.starter_engaged);
    CHECK(r.plant.rpm == 0.0);
}

static void test_speed_request_followed(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    rig_run_requesting(&r, 6.0, 1500.0);
    CHECK(r.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    CHECK(r.ecu.controlling_address == MC_ADDR);
    CHECK_NEAR(r.plant.rpm, 1500.0, 5.0);
}

static void test_speed_request_clamped(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    rig_run_requesting(&r, 6.0, 3000.0);
    CHECK_NEAR(r.ecu.target_rpm, EP->high_idle_rpm, 1e-9);
    CHECK_NEAR(r.plant.rpm, EP->high_idle_rpm, 5.0);
    CHECK(r.max_rpm < EP->overspeed_trip_rpm);

    rig_run_requesting(&r, 6.0, 300.0);
    CHECK_NEAR(r.ecu.target_rpm, EP->low_idle_rpm, 1e-9);
    CHECK_NEAR(r.plant.rpm, EP->low_idle_rpm, 5.0);
}

/* A stale request is ignored: the engine returns to idle on its own. */
static void test_request_timeout(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    rig_run_requesting(&r, 6.0, 1500.0);

    rig_run(&r, (double)(EP->request_timeout_ms - DT_MS) / 1000.0);
    CHECK(r.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    rig_run(&r, 2.0 * (double)DT_MS / 1000.0);
    CHECK(r.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    CHECK(r.ecu.controlling_address == EP->source_address);
    rig_run(&r, 6.0);
    CHECK_NEAR(r.plant.rpm, EP->low_idle_rpm, 5.0);
}

static void test_non_speed_modes_mean_idle(void) {
    const EngineEcuOverride modes[] = { ECU_OVERRIDE_DISABLED, ECU_OVERRIDE_TORQUE, ECU_OVERRIDE_SPEED_TORQUE_LIMIT };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        Rig r;
        rig_init(&r);
        rig_start(&r);
        const EngineEcuRequest req = { .mode = modes[i], .speed_valid = true, .speed_rpm = 1500.0, .source_address = MC_ADDR };
        for (int k = 0; k < 300; k++) {
            engine_ecu_request(&r.ecu, &req);
            rig_step(&r);
        }
        CHECK(r.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
        CHECK_NEAR(r.plant.rpm, EP->low_idle_rpm, 5.0);
    }
}

static void test_ignore_requests_fault(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    r.ecu.ignore_requests = true;
    rig_run_requesting(&r, 3.0, 1500.0);
    CHECK(r.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    CHECK_NEAR(r.plant.rpm, EP->low_idle_rpm, 5.0);
}

/* Overspeed cuts fuel and stays latched until the engine has stopped. */
static void test_overspeed_trip_latched_until_stop(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    rig_run_requesting(&r, 4.0, 1500.0);

    r.load_nm = -700.0; /* overrunning load drives the engine */
    int steps = 0;
    while (!r.ecu.overspeed_tripped && steps < 1000) {
        engine_ecu_request(&r.ecu, &(EngineEcuRequest){ ECU_OVERRIDE_SPEED, true, 1500.0, MC_ADDR });
        rig_step(&r);
        steps++;
    }
    CHECK(r.ecu.overspeed_tripped);
    CHECK(r.plant.rpm > EP->overspeed_trip_rpm);
    r.load_nm = 0.0;

    /* Speed falls back through the normal range: still tripped, still no fuel. */
    int fuel_seen = 0;
    for (int i = 0; i < 1500 && r.plant.rpm > 0.0; i++) {
        engine_ecu_request(&r.ecu, &(EngineEcuRequest){ ECU_OVERRIDE_SPEED, true, 1500.0, MC_ADDR });
        rig_step(&r);
        if (r.plant.rpm > 1.0 && (!r.ecu.overspeed_tripped || r.plant.fuel_pct > 0.0)) {
            fuel_seen = 1;
        }
    }
    CHECK(!fuel_seen);
    CHECK(r.plant.rpm == 0.0);
    rig_step(&r);
    CHECK(!r.ecu.overspeed_tripped);
    CHECK(r.ecu.state == ECU_STATE_STOPPED);

    /* It does not restart by itself. */
    rig_run(&r, 3.0);
    CHECK(r.plant.rpm == 0.0);
}

static void test_stall_detected(void) {
    Rig r;
    rig_init(&r);
    rig_start(&r);
    r.load_nm = 1500.0;
    rig_run(&r, 3.0);
    CHECK(r.ecu.state == ECU_STATE_STOPPED);
    CHECK(r.ecu.fuel_pct == 0.0);
    CHECK(r.plant.rpm == 0.0);
}

static void test_droop(void) {
    EngineEcuParams drooping = *EP;
    drooping.droop_pct = 5.0;

    EngineEcu e;
    PlantState s;
    engine_ecu_init(&e, &drooping);
    plant_init(&s, PP);
    EngineEcuInputs in = { .key_on = true, .start = true, .rpm = 0.0 };
    const EngineEcuRequest req = { ECU_OVERRIDE_SPEED, true, 1500.0, MC_ADDR };
    double load = 0.0;
    for (int i = 0; i < 2000; i++) {
        in.start = i < 300;
        load = (i > 1000) ? 600.0 : 0.0;
        if (i > 1000) {
            engine_ecu_request(&e, &req);
        }
        EngineEcuOutputs out;
        in.rpm = s.rpm;
        engine_ecu_step(&e, &drooping, &in, DT_MS, &out);
        PlantInputs pin = { out.fuel_pct, load, out.starter_engaged };
        plant_step(&s, PP, &pin, 0.010);
    }
    const double expected = 1500.0 * (1.0 - 0.05 * e.fuel_pct / 100.0);
    CHECK_NEAR(s.rpm, expected, 3.0);
    CHECK(s.rpm < 1500.0 - 20.0);

    /* Same case isochronous: back on the setpoint. */
    Rig r;
    rig_init(&r);
    rig_start(&r);
    r.load_nm = 600.0;
    rig_run_requesting(&r, 8.0, 1500.0);
    CHECK_NEAR(r.plant.rpm, 1500.0, 3.0);
}

int main(void) {
    RUN_TEST(test_key_off_means_no_fuel);
    RUN_TEST(test_no_fuel_below_enable_speed);
    RUN_TEST(test_start_and_idle);
    RUN_TEST(test_releasing_start_early_aborts);
    RUN_TEST(test_speed_request_followed);
    RUN_TEST(test_speed_request_clamped);
    RUN_TEST(test_request_timeout);
    RUN_TEST(test_non_speed_modes_mean_idle);
    RUN_TEST(test_ignore_requests_fault);
    RUN_TEST(test_overspeed_trip_latched_until_stop);
    RUN_TEST(test_stall_detected);
    RUN_TEST(test_droop);
    return check_report();
}
