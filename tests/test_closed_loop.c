/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_closed_loop.c
 * @brief The v0.1 exit scenarios, closed loop: reference machine controller,
 *        J1939 over an in-memory bus, simulated engine ECU, plant and pump.
 *
 * Cold start to a target speed; load step and release; loss of the speed
 * command; loss of engine status; overspeed protection; an engine that
 * ignores the override; determinism.
 */

#include <string.h>

#include "check.h"
#include <opencrank/plants/d936la6.h>
#include <opencrank/runtime/loop.h>

#ifndef OC_SCENARIO_DIR
#error "OC_SCENARIO_DIR must point at the scenarios/ directory"
#endif

/* The scenario files are the same ones the SIL demo and open-crank-run use.
 * In each, the engine is started and asked for 1500 rpm, settled by ~6 s. */
#define COLD_START "cold_start.csv"
#define HOIST_STEP "hoist_step.csv"
#define SPEED_COMMAND_LOSS "speed_command_loss.csv"
#define ENGINE_STATUS_LOSS "engine_status_loss.csv"
#define OVERSPEED "overspeed.csv"
#define OVERRIDE_IGNORED "override_ignored.csv"
#define SPEED_COMMAND_CORRUPT "speed_command_corrupt.csv"

/* What happened during a stretch of simulated time. */
typedef struct {
    double min_rpm;
    double max_rpm;
    bool mc_fault_seen;
    McFault first_fault;
    bool tripped_seen;
    bool fuel_while_tripped;
} Window;

static OcScenario scn;
static OcLoop loop;

static void start(const char *file) {
    char path[512];
    char err[600] = "";
    snprintf(path, sizeof(path), "%s/%s", OC_SCENARIO_DIR, file);
    const bool ok = oc_scenario_load(path, &scn, err, sizeof(err));
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  scenario: %s\n", err);
    }
    oc_loop_init(&loop, &OC_BENCH_D936LA6, &MC_DEFAULT_PARAMS, &scn);
}

static double now_s(void) {
    return oc_bench_time_s(&loop.bench);
}

static double rpm(void) {
    return loop.bench.plant.rpm;
}

/* Runs until simulated time t_s, recording into w (if given). */
static void run_to(double t_s, Window *w) {
    if (w != NULL) {
        w->min_rpm = 1e9;
        w->max_rpm = 0.0;
        w->mc_fault_seen = false;
        w->tripped_seen = false;
        w->fuel_while_tripped = false;
    }
    while (now_s() < t_s - 1e-9) {
        oc_loop_step(&loop);
        CHECK(loop.queue_overflows == 0U);
        if (w == NULL) {
            continue;
        }
        if (rpm() < w->min_rpm) {
            w->min_rpm = rpm();
        }
        if (rpm() > w->max_rpm) {
            w->max_rpm = rpm();
        }
        if (loop.mc.state == MC_STATE_FAULT && !w->mc_fault_seen) {
            w->mc_fault_seen = true;
            w->first_fault = loop.mc.fault;
        }
        if (loop.bench.ecu.overspeed_tripped) {
            w->tripped_seen = true;
            if (loop.bench.plant.fuel_pct > 0.0) {
                w->fuel_while_tripped = true;
            }
        }
    }
}

static void test_cold_start(void) {
    Window w;
    start(COLD_START);
    run_to(0.5, &w);
    CHECK(w.max_rpm == 0.0); /* nothing moves before START */
    run_to(3.0, &w);
    CHECK(loop.bench.ecu.state == ECU_STATE_RUNNING);
    CHECK(loop.mc.state == MC_STATE_RUN);
    run_to(12.0, &w);
    CHECK(!w.mc_fault_seen);
    CHECK_NEAR(rpm(), 1500.0, 5.0);
    CHECK(w.max_rpm < 1550.0);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    CHECK(loop.bench.ecu.controlling_address == MC_DEFAULT_PARAMS.source_address);
}

static void test_load_step_and_release(void) {
    Window w;
    start(HOIST_STEP);
    run_to(8.0, NULL);
    CHECK_NEAR(rpm(), 1500.0, 5.0);

    run_to(11.0, &w); /* load applied at 8 s */
    CHECK(w.min_rpm > 1500.0 - 250.0);
    CHECK_NEAR(rpm(), 1500.0, 20.0);
    run_to(17.9, NULL);
    CHECK_NEAR(rpm(), 1500.0, 5.0);
    CHECK_NEAR(loop.bench.load_torque_nm, 568.0, 10.0); /* 85 % of 140 cm3 at 270 bar */

    run_to(21.0, &w); /* released at 18 s */
    CHECK(w.max_rpm < 1500.0 + 250.0);
    CHECK_NEAR(rpm(), 1500.0, 20.0);
    run_to(25.0, &w);
    CHECK_NEAR(rpm(), 1500.0, 5.0);
    CHECK(loop.mc.state == MC_STATE_RUN);
    CHECK(loop.bench.ecu.state == ECU_STATE_RUNNING);
}

/* The controller's frames stop arriving: the engine must drop the stale
 * request and return to idle; the controller must notice it lost control. */
/* SR-5 */
static void test_speed_command_loss(void) {
    Window w;
    start(SPEED_COMMAND_LOSS);
    run_to(8.0, NULL);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_REQUEST);

    run_to(8.0 + 0.25, &w);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    run_to(9.0, &w);
    CHECK(loop.mc.state == MC_STATE_FAULT);
    CHECK(loop.mc.fault == MC_FAULT_OVERRIDE_REJECTED);
    run_to(13.9, &w);
    CHECK_NEAR(rpm(), 750.0, 10.0);

    /* Link back: still FAULT until the operator resets. */
    run_to(14.4, &w);
    CHECK(loop.mc.state == MC_STATE_FAULT);
    run_to(16.5, &w);
    CHECK(loop.mc.state == MC_STATE_RUN);
    run_to(25.0, &w);
    CHECK_NEAR(rpm(), 1500.0, 5.0);
}

/* The engine goes silent: the controller faults and releases the override;
 * the pump command drops to zero; the engine returns to idle. */
/* The controller's frames arrive with the high byte of the requested speed
 * flipped (1500 rpm would read as about 6716): the engine must refuse them
 * by their checksum, never speed up, and the controller must release. */
/* SR-10 */
static void test_speed_command_corrupt(void) {
    Window w;
    start(SPEED_COMMAND_CORRUPT);
    run_to(8.0, NULL);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    CHECK(loop.bench.tsc1_rejected == 0U);

    run_to(14.0, &w);
    CHECK(w.max_rpm < 1600.0); /* without the checksum: about 2100, the governed ceiling */
    CHECK(loop.bench.tsc1_rejected > 500U); /* every TSC1 for 6 s */
    CHECK(w.mc_fault_seen && w.first_fault == MC_FAULT_OVERRIDE_REJECTED);
    CHECK_NEAR(rpm(), 750.0, 10.0);

    run_to(25.0, &w);
    CHECK(loop.mc.state == MC_STATE_RUN);
    CHECK_NEAR(rpm(), 1500.0, 5.0);
}

/* SR-4 */
static void test_engine_status_loss(void) {
    Window w;
    start(ENGINE_STATUS_LOSS);
    run_to(8.0, NULL);
    CHECK(loop.mc.state == MC_STATE_RUN);
    CHECK_NEAR(loop.bench.pump_cmd_applied_pct, 50.0, 0.5);

    run_to(8.0 + 0.15, &w);
    CHECK(loop.mc.state == MC_STATE_RUN);
    run_to(8.0 + 0.3, &w);
    CHECK(loop.mc.state == MC_STATE_FAULT);
    CHECK(loop.mc.fault == MC_FAULT_ENGINE_STATUS_LOST);
    run_to(8.6, &w);
    CHECK(loop.bench.pump_cmd_applied_pct == 0.0);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    run_to(14.0, &w);
    CHECK_NEAR(rpm(), 750.0, 10.0);
}

/* An overrunning load drives the engine past its limits. */
/* SR-6 */
static void test_overspeed_protection(void) {
    Window w;
    start(OVERSPEED);
    run_to(8.0, NULL);
    CHECK(loop.mc.state == MC_STATE_RUN);

    run_to(25.0, &w);
    CHECK(w.max_rpm > ENGINE_ECU_D936LA6.overspeed_trip_rpm);
    CHECK(w.tripped_seen);
    CHECK(!w.fuel_while_tripped);
    CHECK(w.mc_fault_seen);
    CHECK(w.first_fault == MC_FAULT_OVERSPEED);
    /* Latched until standstill: the engine stopped instead of returning to idle. */
    CHECK(rpm() == 0.0);
    CHECK(loop.bench.ecu.state == ECU_STATE_STOPPED);
    CHECK(!loop.bench.ecu.overspeed_tripped);
    CHECK(loop.mc.state == MC_STATE_FAULT);
}

/* The engine never honours the override: the controller must not believe it does. */
/* SR-5 */
static void test_override_ignored(void) {
    Window w;
    start(OVERRIDE_IGNORED);
    run_to(10.0, &w);
    CHECK(w.mc_fault_seen);
    CHECK(w.first_fault == MC_FAULT_OVERRIDE_REJECTED);
    CHECK(loop.bench.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    CHECK_NEAR(rpm(), 750.0, 10.0);
}

/* Same scenario, same result, bit for bit. */
static void test_deterministic(void) {
    static OcLoop first;
    start(HOIST_STEP);
    run_to(25.0, NULL);
    first = loop;
    start(HOIST_STEP);
    run_to(25.0, NULL);
    CHECK(memcmp(&first.bench.plant, &loop.bench.plant, sizeof(loop.bench.plant)) == 0);
    CHECK(memcmp(&first.bench.ecu, &loop.bench.ecu, sizeof(loop.bench.ecu)) == 0);
    CHECK(first.mc.requested_rpm == loop.mc.requested_rpm);
}

int main(void) {
    RUN_TEST(test_cold_start);
    RUN_TEST(test_load_step_and_release);
    RUN_TEST(test_speed_command_loss);
    RUN_TEST(test_speed_command_corrupt);
    RUN_TEST(test_engine_status_loss);
    RUN_TEST(test_overspeed_protection);
    RUN_TEST(test_override_ignored);
    RUN_TEST(test_deterministic);
    return check_report();
}
