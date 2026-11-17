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

/* Engine started and asked for 1500 rpm; settled by about 6 s. */
#define PREFIX                       \
    "time_s,signal,value\n"          \
    "0.0,key_on,1\n"                 \
    "0.0,target_rpm,1500\n"          \
    "0.0,hoist_load_bar,250\n"       \
    "0.5,start,1\n"                  \
    "3.0,start,0\n"

static const char *const COLD_START = PREFIX "12.0,end,0\n";

static const char *const HOIST_STEP = PREFIX
    "8.0,hoist_demand_pct,85\n"
    "18.0,hoist_demand_pct,0\n"
    "25.0,end,0\n";

static const char *const SPEED_COMMAND_LOSS = PREFIX
    "8.0,fault.mc_tx_lost,1\n"
    "14.0,fault.mc_tx_lost,0\n"
    "14.5,fault_reset,1\n"
    "15.0,fault_reset,0\n"
    "25.0,end,0\n";

static const char *const ENGINE_STATUS_LOSS = PREFIX
    "6.0,hoist_demand_pct,50\n"
    "8.0,fault.engine_tx_lost,1\n"
    "14.0,end,0\n";

static const char *const OVERSPEED = PREFIX
    "8.0,fault.overrun_torque_nm,400\n"
    "9.0,fault.overrun_torque_nm,0\n"
    "25.0,end,0\n";

static const char *const OVERRIDE_IGNORED =
    "time_s,signal,value\n"
    "0.0,fault.ecu_ignores_tsc1,1\n"
    "0.0,key_on,1\n"
    "0.0,target_rpm,1500\n"
    "0.5,start,1\n"
    "3.0,start,0\n"
    "10.0,end,0\n";

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

static void start(const char *text) {
    char err[160] = "";
    const bool ok = oc_scenario_parse(text, &scn, err, sizeof(err));
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
    RUN_TEST(test_engine_status_loss);
    RUN_TEST(test_overspeed_protection);
    RUN_TEST(test_override_ignored);
    RUN_TEST(test_deterministic);
    return check_report();
}
