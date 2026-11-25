/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_machine_ctrl.c
 * @brief Machine-controller core against a scripted engine: state machine,
 *        timeouts, slew limit, override confirmation, outputs.
 */

#include <string.h>
#include <stdint.h>

#include "check.h"
#include <opencrank/j1939_diag.h>
#include <opencrank/j1939_diag_tx.h>
#include <opencrank/machine_ctrl.h>

static const McParams *const P = &MC_DEFAULT_PARAMS;

/* A scripted engine and operator around the controller. */
typedef struct {
    McCtx mc;
    uint32_t now_ms;
    float engine_rpm;
    bool engine_sends;         /* EEC1 on the bus */
    bool engine_speed_na;      /* EEC1 sent, speed "not available" */
    bool engine_honours;       /* reports the controller in SPN 1483 when asked */
    bool operator_sends;
    float target_rpm;
    float hoist_pct;
    bool reset_pressed;
    J1939Tsc1 tsc1;            /* last TSC1 sent by the controller */
    OcPumpCmd pump;            /* last pump command */
    uint32_t tsc1_count;
} Harness;

static void h_init(Harness *h, uint32_t start_ms) {
    mc_init(&h->mc, P);
    h->now_ms = start_ms;
    h->engine_rpm = 0.0f;
    h->engine_sends = true;
    h->engine_speed_na = false;
    h->engine_honours = true;
    h->operator_sends = true;
    h->target_rpm = 1500.0f;
    h->hoist_pct = 0.0f;
    h->reset_pressed = false;
    h->tsc1_count = 0U;
    h->tsc1.override_mode = J1939_OVERRIDE_DISABLED;
}

static void h_tick(Harness *h) {
    J1939Frame f;
    if (h->engine_sends) {
        const bool in_control = h->engine_honours && h->tsc1.override_mode == J1939_OVERRIDE_SPEED;
        const J1939Eec1 e = { 0, true, 10.0f, true, 10.0f, !h->engine_speed_na, h->engine_rpm,
                              in_control ? P->source_address : J1939_ADDR_ENGINE_1 };
        j1939_encode_eec1(&e, J1939_ADDR_ENGINE_1, &f);
        mc_handle_frame(&h->mc, &f, h->now_ms);
    }
    if (h->operator_sends) {
        const OcOperator op = { true, h->target_rpm, true, h->hoist_pct, h->reset_pressed };
        oc_encode_operator(&op, OC_ADDR_BENCH, &f);
        mc_handle_frame(&h->mc, &f, h->now_ms);
    }

    J1939Frame out[4];
    const size_t n = mc_step(&h->mc, h->now_ms, out, 4);
    for (size_t i = 0; i < n; i++) {
        if (j1939_decode_tsc1(&out[i], &h->tsc1)) {
            h->tsc1_count++;
            CHECK(out[i].id == 0x0C000027UL);
        }
        (void)oc_decode_pump_cmd(&out[i], &h->pump);
    }
    h->now_ms += 10U;
}

static void h_run(Harness *h, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10U) {
        h_tick(h);
    }
}

/* Engine at idle; ticks until the controller has just entered RUN. */
static void h_to_run(Harness *h) {
    h->engine_rpm = 750.0f;
    for (int i = 0; i < 200 && h->mc.state != MC_STATE_RUN; i++) {
        h_tick(h);
    }
    CHECK(h->mc.state == MC_STATE_RUN);
}

/* SR-1 */
static void test_waits_for_engine(void) {
    Harness h;
    h_init(&h, 0U);
    h_run(&h, 2000U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
    CHECK(h.tsc1_count == 200U); /* sends every period */
    CHECK(h.tsc1.override_mode == J1939_OVERRIDE_DISABLED);
    CHECK(!h.tsc1.requested_speed_valid);
    CHECK(h.pump.displacement_valid && h.pump.displacement_pct == 0.0f);
}

/* SR-1 */
static void test_run_after_confirmation(void) {
    Harness h;
    h_init(&h, 0U);
    h.engine_rpm = 750.0f;
    h_run(&h, P->run_confirm_ms - 100U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
    h_run(&h, 200U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK(h.tsc1.override_mode == J1939_OVERRIDE_SPEED);

    /* A flicker below running speed restarts the confirmation. */
    Harness g;
    h_init(&g, 0U);
    g.engine_rpm = 750.0f;
    h_run(&g, 300U);
    g.engine_rpm = 500.0f;
    h_run(&g, 20U);
    g.engine_rpm = 750.0f;
    h_run(&g, 300U);
    CHECK(g.mc.state == MC_STATE_WAIT_ENGINE);
}

static void test_request_is_slew_limited(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    const float start = h.mc.requested_rpm;
    CHECK_NEAR(start, 750.0, 10.0);
    h_run(&h, 1000U);
    CHECK_NEAR(h.mc.requested_rpm, start + P->slew_rpm_per_s * 1.0f, 10.0);
    CHECK_NEAR(h.tsc1.requested_speed_rpm, h.mc.requested_rpm, 0.125);
    h_run(&h, 1000U);
    CHECK_NEAR(h.mc.requested_rpm, 1500.0, 0.01);
}

/* SR-2 */
static void test_request_clamped(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.target_rpm = 5000.0f;
    h_run(&h, 5000U);
    CHECK_NEAR(h.mc.requested_rpm, P->request_max_rpm, 0.01);
    h.target_rpm = 100.0f;
    h_run(&h, 5000U);
    CHECK_NEAR(h.mc.requested_rpm, P->request_min_rpm, 0.01);
}

/* SR-3 */
static void test_hoist_only_in_run(void) {
    Harness h;
    h_init(&h, 0U);
    h.hoist_pct = 60.0f;
    h_run(&h, 200U);
    CHECK(h.pump.displacement_pct == 0.0f);
    h_to_run(&h);
    CHECK_NEAR(h.pump.displacement_pct, 60.0, 0.4);
    h.engine_sends = false;
    h_run(&h, 500U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.pump.displacement_pct == 0.0f);
}

/* SR-4 */
static void test_engine_status_timeout(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.engine_sends = false;
    h_run(&h, P->eec1_timeout_ms - 50U);
    CHECK(h.mc.state == MC_STATE_RUN);
    h_run(&h, 100U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.mc.fault == MC_FAULT_ENGINE_STATUS_LOST);
    CHECK(h.tsc1.override_mode == J1939_OVERRIDE_DISABLED);

    /* Speed "not available" is no status either. */
    Harness g;
    h_init(&g, 0U);
    h_to_run(&g);
    g.engine_speed_na = true;
    h_run(&g, P->eec1_timeout_ms + 50U);
    CHECK(g.mc.fault == MC_FAULT_ENGINE_STATUS_LOST);
}

/* SR-6 */
static void test_overspeed_and_stall(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.engine_rpm = P->overspeed_rpm + 50.0f;
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.mc.fault == MC_FAULT_OVERSPEED);

    Harness s;
    h_init(&s, 0U);
    h_to_run(&s);
    s.engine_rpm = P->stall_rpm - 50.0f;
    h_run(&s, 20U);
    CHECK(s.mc.fault == MC_FAULT_STALL);
}

/* SR-5 */
static void test_override_must_be_honoured(void) {
    Harness h;
    h_init(&h, 0U);
    h.engine_honours = false;
    h.engine_rpm = 750.0f;
    h_run(&h, P->run_confirm_ms + 20U);
    CHECK(h.mc.state == MC_STATE_RUN);
    h_run(&h, P->override_confirm_ms - 50U);
    CHECK(h.mc.state == MC_STATE_RUN);
    h_run(&h, 100U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.mc.fault == MC_FAULT_OVERRIDE_REJECTED);
}

/* SR-11 */
/* After a watchdog restart the controller starts in FAULT, in the safe
 * state, and only a new press of the fault reset lets it take control again. */
static void test_starts_in_fault_after_watchdog(void) {
    Harness h;
    h_init(&h, 0U);
    mc_init_after_watchdog(&h.mc, P);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.mc.fault == MC_FAULT_WATCHDOG_RESET);

    h.engine_rpm = 1500.0f; /* the engine is still running */
    h.hoist_pct = 60.0f;
    h.reset_pressed = true; /* held through the restart: not a new press */
    h_run(&h, 2000U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    CHECK(h.tsc1.override_mode == J1939_OVERRIDE_DISABLED);
    CHECK(h.pump.displacement_valid && h.pump.displacement_pct == 0.0f);

    h.reset_pressed = false;
    h_run(&h, 100U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    h.reset_pressed = true; /* a new press */
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
    h_run(&h, P->run_confirm_ms + 50U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK(strcmp(mc_fault_name(MC_FAULT_WATCHDOG_RESET), "watchdog reset") == 0);
}

/* The engine's DM1 with these lamps (and one code each), in one frame, or as
 * a BAM when both are on, delivered at the harness's current time. */
static void h_dm1(Harness *h, bool red, bool amber) {
    J1939Dm1 m = { 0 };
    m.red_stop = red ? J1939_LAMP_ON : J1939_LAMP_OFF;
    m.amber_warning = amber ? J1939_LAMP_ON : J1939_LAMP_OFF;
    if (red) {
        m.dtc[m.n_dtc++] = (J1939Dtc){ 100U, 1U, 1U };
    }
    if (amber) {
        m.dtc[m.n_dtc++] = (J1939Dtc){ 110U, 16U, 1U };
    }
    uint8_t d[J1939_DM1_MAX_LEN];
    const size_t len = j1939_encode_dm1(&m, d, sizeof(d));
    J1939Frame fr[J1939_TP_MAX_PACKETS + 1U];
    if (len == 8U) {
        fr[0].id = j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_DM1, J1939_ADDR_GLOBAL, J1939_ADDR_ENGINE_1);
        fr[0].dlc = 8U;
        memcpy(fr[0].data, d, 8);
        mc_handle_frame(&h->mc, &fr[0], h->now_ms);
        return;
    }
    const size_t n = j1939_bam_encode(J1939_PGN_DM1, d, len, J1939_ADDR_ENGINE_1, fr, J1939_TP_MAX_PACKETS + 1U);
    for (size_t i = 0U; i < n; i++) {
        mc_handle_frame(&h->mc, &fr[i], h->now_ms);
        if (i + 1U < n) {
            h_run(h, J1939_TP_BAM_GAP_MS);
        }
    }
}

/* SR-12 */
static void test_red_stop_lamp(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.hoist_pct = 50.0f;
    h_run(&h, 100U);
    h_dm1(&h, true, false);
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_FAULT && h.mc.fault == MC_FAULT_ENGINE_RED_STOP);
    CHECK(h.tsc1.override_mode == J1939_OVERRIDE_DISABLED && h.pump.displacement_pct == 0.0f);

    /* A fault reset while the lamp is still on: no RUN. */
    h.reset_pressed = true;
    h_run(&h, 20U);
    h.reset_pressed = false;
    h_run(&h, 2000U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
    h_dm1(&h, false, false);
    h_run(&h, P->run_confirm_ms + 50U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK(strcmp(mc_fault_name(MC_FAULT_ENGINE_RED_STOP), "engine red stop lamp") == 0);
}

/* SR-12: two codes, so a BAM; the red lamp in it still faults. */
static void test_red_stop_lamp_over_bam(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h_dm1(&h, true, true);
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_FAULT && h.mc.fault == MC_FAULT_ENGINE_RED_STOP);
    CHECK(h.mc.engine_amber);
}

/* SR-13: limp-home, reached and left smoothly, and never a fault. */
static void test_limp_home(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.hoist_pct = 85.0f;
    h_run(&h, 3000U); /* at 1500 rpm, pump 85 % */
    CHECK_NEAR(h.mc.requested_rpm, 1500.0, 0.01);
    CHECK_NEAR(h.pump.displacement_pct, 85.0, 0.4);

    h_dm1(&h, false, true);
    float last_rpm = h.mc.requested_rpm;
    float last_pump = h.pump.displacement_pct;
    for (int i = 0; i < 100; i++) { /* 1 s, step by step */
        h_run(&h, 10U);
        /* smooth: at most 500 rpm/s and 100 %/s (+ one bit of OC_PUMP_CMD) per 10 ms */
        CHECK(last_rpm - h.mc.requested_rpm <= 5.0f + 0.01f);
        CHECK(last_pump - h.pump.displacement_pct <= 1.0f + 0.4f);
        last_rpm = h.mc.requested_rpm;
        last_pump = h.pump.displacement_pct;
    }
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK_NEAR(h.mc.requested_rpm, P->limp_rpm, 0.01);
    CHECK_NEAR(h.pump.displacement_pct, P->limp_pump_pct, 0.4);

    /* Releasing the lever is not slowed by the limit (SR-8). */
    h.hoist_pct = 0.0f;
    h_run(&h, 20U);
    CHECK(h.pump.displacement_pct == 0.0f);
    h.hoist_pct = 85.0f;
    h_run(&h, 20U);
    CHECK_NEAR(h.pump.displacement_pct, P->limp_pump_pct, 0.4);

    /* The lamp goes off: back to the operator's values, ramped, on its own. */
    h_dm1(&h, false, false);
    h_run(&h, 100U);
    CHECK(h.pump.displacement_pct < 62.0f); /* +10 % per 100 ms, not a step */
    h_run(&h, 1500U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK_NEAR(h.mc.requested_rpm, 1500.0, 0.01);
    CHECK_NEAR(h.pump.displacement_pct, 85.0, 0.4);
}

/* SR-7 */
static void test_reset_on_press_only(void) {
    Harness h;
    h_init(&h, 0U);
    h_to_run(&h);
    h.engine_sends = false;
    h_run(&h, 500U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    h.engine_sends = true;
    h_run(&h, 500U);
    CHECK(h.mc.state == MC_STATE_FAULT); /* no automatic recovery */

    h.reset_pressed = true;
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
    CHECK(h.mc.fault == MC_FAULT_NONE);
    h_run(&h, 1000U); /* button still held */
    CHECK(h.mc.state == MC_STATE_RUN);

    /* Holding the button does not clear a new fault: it needs a new press. */
    h.engine_sends = false;
    h_run(&h, 500U);
    CHECK(h.mc.state == MC_STATE_FAULT);
    h.engine_sends = true;
    h_run(&h, 500U);
    CHECK(h.mc.state == MC_STATE_FAULT);

    mc_clear_fault(&h.mc);
    h_run(&h, 20U);
    CHECK(h.mc.state == MC_STATE_WAIT_ENGINE);
}

static void test_operator_timeout(void) {
    Harness h;
    h_init(&h, 0U);
    h.hoist_pct = 80.0f;
    h_to_run(&h);
    h_run(&h, 3000U);
    CHECK_NEAR(h.mc.requested_rpm, 1500.0, 0.01);
    h.operator_sends = false;
    h_run(&h, 3000U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK_NEAR(h.mc.requested_rpm, P->request_min_rpm, 0.01);
    CHECK(h.pump.displacement_pct == 0.0f);
}

static void test_time_wraps(void) {
    Harness h;
    h_init(&h, UINT32_MAX - 700U);
    h_to_run(&h); /* crosses the wrap */
    h_run(&h, 1000U);
    CHECK(h.mc.state == MC_STATE_RUN);
    CHECK_NEAR(h.mc.requested_rpm, 1250.0, 15.0);
    h.engine_sends = false;
    h_run(&h, 300U);
    CHECK(h.mc.fault == MC_FAULT_ENGINE_STATUS_LOST);
}

int main(void) {
    RUN_TEST(test_waits_for_engine);
    RUN_TEST(test_run_after_confirmation);
    RUN_TEST(test_request_is_slew_limited);
    RUN_TEST(test_request_clamped);
    RUN_TEST(test_hoist_only_in_run);
    RUN_TEST(test_engine_status_timeout);
    RUN_TEST(test_overspeed_and_stall);
    RUN_TEST(test_override_must_be_honoured);
    RUN_TEST(test_reset_on_press_only);
    RUN_TEST(test_red_stop_lamp);
    RUN_TEST(test_red_stop_lamp_over_bam);
    RUN_TEST(test_limp_home);
    RUN_TEST(test_starts_in_fault_after_watchdog);
    RUN_TEST(test_operator_timeout);
    RUN_TEST(test_time_wraps);
    return check_report();
}
