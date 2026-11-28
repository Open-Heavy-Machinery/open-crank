/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_l2_loop.c
 * @brief The Level 2 loop in memory: the edge model against the plant's
 *        angle, the HSI's bias in the ECU's speed, the DUT-to-plant delay, an
 *        injection end to end, a lost tooth, the observables, determinism.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include <opencrank/can_otk.h>
#include <opencrank/l2/l2_accept.h>
#include <opencrank/l2/l2_loop.h>

/* A scenario in memory: hold @p rpm from rest, then optionally lose a tooth at 8 s. */
static void scenario(OcScenario *s, double rpm, int lose_tooth) {
    char text[512];
    (void)snprintf(text, sizeof(text),
                   "time_s,signal,value\n0.0,key_on,1\n0.0,target_rpm,%.0f\n0.5,start,1\n3.0,start,0\n%s12.0,end,0\n",
                   rpm, lose_tooth ? "8.0,fault.crank_teeth_lost,1\n" : "");
    char err[256];
    CHECK(otk_scenario_parse(text, &OC_SIGNAL_TABLE, s, err, sizeof(err)));
}

static OcL2Loop L, M;

static void run_until(OcL2Loop *l, uint32_t ms) {
    OtkCanFrame tx[4];
    while (l->t_ms < ms && !oc_l2_loop_done(l)) {
        (void)oc_l2_loop_step(l, tx, 4U);
    }
}

/* In FULL sync, the tooth the ECU believes it last saw is the one at the plant's angle. */
static void test_edges_at_the_plants_angle(void) {
    OcScenario s;
    scenario(&s, 1500.0, 0);
    OcL2LoopConfig cfg = OC_L2_LOOP_D936LA6;
    cfg.hsi_bias = 0.0;
    oc_l2_loop_init(&L, &cfg, &s);
    run_until(&L, 6000U);
    int checked = 0, wrong = 0;
    OtkCanFrame tx[4];
    for (int k = 0; k < 2000; k++) {
        (void)oc_l2_loop_step(&L, tx, 4U);
        const double a = oc_l2_acc_deg(L.plant.acc);         /* the angle at the end of this step */
        const double rel = fmod(a + 60.0 + 720.0, 360.0);     /* from tooth 0 of this revolution */
        const double pos = rel / 6.0;
        if (pos >= 58.0 || fabs(pos - floor(pos + 1e-9)) < 1e-3) {
            continue; /* in the gap, or on an edge: ambiguous */
        }
        checked++;
        if (L.wheel.st.sync != ECU_SYNC_FULL || L.wheel.st.tooth != (uint8_t)floor(pos)) {
            wrong++;
        }
        const unsigned rev = fmod(a + 60.0 + 720.0, 720.0) >= 360.0 ? 1u : 0u;
        if (L.wheel.st.rev != rev) {
            wrong++;
        }
    }
    CHECK(checked > 1500);
    CHECK(wrong == 0);
}

/* The ECU reads the speed with its own clock: equal with no bias, 0.27 % high with the HSI's. */
static void test_hsi_bias_in_the_ecus_speed(void) {
    OcScenario s;
    scenario(&s, 1500.0, 0);
    OcL2LoopConfig cfg = OC_L2_LOOP_D936LA6;
    cfg.hsi_bias = 0.0;
    oc_l2_loop_init(&L, &cfg, &s);
    run_until(&L, 8000U);
    CHECK_NEAR((double)L.wheel.st.rpm / L.plant.plant.rpm, 1.0, 1e-4);
    CHECK_NEAR(L.plant.plant.rpm, 1500.0, 0.5);

    oc_l2_loop_init(&L, &OC_L2_LOOP_D936LA6, &s);
    run_until(&L, 8000U);
    CHECK_NEAR((double)L.wheel.st.rpm / L.plant.plant.rpm, 1.0 / (1.0 - 2658e-6), 1e-4);
    CHECK_NEAR((double)L.wheel.st.rpm, 1500.0, 0.5); /* the ECU holds what it reads */
    CHECK(L.plant.plant.rpm < 1497.0);               /* so the plant runs about 4 rpm low */
}

/* An injection reaches the plant capture_delay_steps after it ends, lands on its angle, and fuels the engine. */
static void test_injection_end_to_end(void) {
    OcScenario s;
    scenario(&s, 1500.0, 0);
    oc_l2_loop_init(&L, &OC_L2_LOOP_D936LA6, &s);
    OtkCanFrame tx[4];
    uint32_t first_cap_due = 0U, first_cap_end_step = 0U;
    while (L.plant.injections == 0U && L.t_ms < 5000U) {
        (void)oc_l2_loop_step(&L, tx, 4U);
        if (first_cap_due == 0U && L.n_cap > 0U) {
            first_cap_due = L.cap[0].due;
            first_cap_end_step = (uint32_t)((L.cap[0].start + L.cap[0].width) / OC_L2_STEP_TICKS);
        }
    }
    CHECK(L.plant.injections == 1u);
    CHECK(first_cap_due == first_cap_end_step + OC_L2_LOOP_D936LA6.capture_delay_steps);
    CHECK(L.plant.step == first_cap_due + 1u); /* delivered at the start of step `due` */
    run_until(&L, 8000U);
    CHECK(L.plant.misplaced == 0u);
    CHECK(L.last.valid && fabs(L.last.soi_err_deg) < 0.05);
    CHECK(L.first_injection_rev > 1.0 && L.first_injection_rev <= 3.5);
    CHECK(L.captures_lost == 0u && L.plant.late_captures == 0u);
}

/* fault.crank_teeth_lost: the ECU loses sync once, stops and resumes; nothing lands in the wrong cylinder. */
static void test_lost_tooth(void) {
    OcScenario s;
    scenario(&s, 1500.0, 1);
    oc_l2_loop_init(&L, &OC_L2_LOOP_D936LA6, &s);
    run_until(&L, 7990U);
    CHECK(L.wheel.st.sync_losses == 0u);
    const uint32_t seen = L.teeth_seen;
    run_until(&L, 8300U);
    CHECK(L.wheel.st.sync_losses == 1u);
    CHECK(L.teeth_to_drop == 0u);
    run_until(&L, 12000U);
    CHECK(L.wheel.st.sync == ECU_SYNC_FULL);
    CHECK(L.plant.misplaced == 0u);
    CHECK(L.misfires_1s == 0u);
    CHECK(L.teeth_seen > seen);
}

/* Each observation frame carries its value, or a gap, and the ECU's EEC1 gives ecu_rpm. */
static void test_observables(void) {
    OcScenario s;
    scenario(&s, 1500.0, 0);
    oc_l2_loop_init(&L, &OC_L2_LOOP_D936LA6, &s);
    L.plant.plant.rpm = 1234.5;
    L.plant.misplaced = 3u;
    L.misfires_1s = 2u;
    L.last.valid = false;
    L.first_injection_seen = false;
    OtkOracleTable table;
    const uint8_t ecu = 0x00u;
    oc_l2_accept_init(&table, &ecu);
    OtkCanFrame f[8];
    const size_t n = oc_l2_observation_frames(&L, f, 8u);
    CHECK(n == 6u);
    OtkOracleValue v[OC_L2_OBS_COUNT];
    for (size_t i = 0; i < OC_L2_OBS_COUNT; i++) {
        v[i] = (OtkOracleValue){ true, -1.0 };
    }
    for (size_t i = 0; i < n; i++) {
        OtkOracleUpdate u[2];
        CHECK(table.decode(table.ctx, &f[i], u, 2u) == 1u);
        v[u[0].observable] = u[0].value;
    }
    CHECK(!v[OC_L2_OBS_PLANT_RPM].gap && v[OC_L2_OBS_PLANT_RPM].value == 1234.5);
    CHECK(!v[OC_L2_OBS_MISPLACED].gap && v[OC_L2_OBS_MISPLACED].value == 3.0);
    CHECK(!v[OC_L2_OBS_MISFIRES_1S].gap && v[OC_L2_OBS_MISFIRES_1S].value == 2.0);
    CHECK(v[OC_L2_OBS_FIRST_INJECTION_REV].gap);
    CHECK(v[OC_L2_OBS_SOI_ERR_DEG].gap);
    CHECK(!v[OC_L2_OBS_FUEL_PCT].gap);

    L.first_injection_seen = true;
    L.first_injection_rev = 2.5;
    L.last.valid = true;
    L.last.soi_err_deg = -0.01;
    (void)oc_l2_observation_frames(&L, f, 8u);
    OtkOracleUpdate u[2];
    CHECK(table.decode(table.ctx, &f[3], u, 2u) == 1u && !u[0].value.gap && u[0].value.value == 2.5);
    CHECK(table.decode(table.ctx, &f[4], u, 2u) == 1u && !u[0].value.gap && u[0].value.value == -0.01);

    /* the ECU's EEC1 */
    const J1939Eec1 m = { .torque_mode = 0u, .engine_speed_valid = true, .engine_speed_rpm = 1501.0f,
                          .controlling_address = 0x27u };
    J1939Frame jf;
    j1939_encode_eec1(&m, 0x00u, &jf);
    OtkCanFrame o;
    oc_frame_to_otk(&jf, &o);
    CHECK(table.decode(table.ctx, &o, u, 2u) == 1u && u[0].observable == OC_L2_OBS_ECU_RPM);
    CHECK_NEAR(u[0].value.value, 1501.0, 0.2);
    /* another node's EEC1 */
    j1939_encode_eec1(&m, 0x01u, &jf);
    oc_frame_to_otk(&jf, &o);
    CHECK(table.decode(table.ctx, &o, u, 2u) == 0u);
}

/* Same scenario, same run, bit for bit. */
static void test_deterministic(void) {
    OcScenario s;
    scenario(&s, 1500.0, 1);
    oc_l2_loop_init(&L, &OC_L2_LOOP_D936LA6, &s);
    oc_l2_loop_init(&M, &OC_L2_LOOP_D936LA6, &s);
    run_until(&L, 12000U);
    run_until(&M, 12000U);
    CHECK(memcmp(&L.plant, &M.plant, sizeof(L.plant)) == 0);
    /* field by field: EcuWheel has padding, which a struct assignment need not copy */
    const EcuWheelStatus *a = &L.wheel.st, *b = &M.wheel.st;
    CHECK(a->sync == b->sync && a->rpm == b->rpm && a->tooth == b->tooth && a->rev == b->rev);
    CHECK(a->teeth == b->teeth && a->cam_edges == b->cam_edges && a->gaps == b->gaps);
    CHECK(a->sync_losses == b->sync_losses && a->stops == b->stops && a->arms == b->arms);
    CHECK(L.wheel.last_t == M.wheel.last_t && L.wheel.last_dt == M.wheel.last_dt && L.wheel.pos == M.wheel.pos);
    CHECK(L.ecu.fuel_pct == M.ecu.fuel_pct && L.ecu.integrator_pct == M.ecu.integrator_pct);
    CHECK(L.plant.plant.rpm == M.plant.plant.rpm);
}

/* With an external ECU: no edge model; the controller's frames come out of the step; a capture handed
 * in is placed by its own tick; an EEC1 from the bus gives ecu_rpm. */
static void test_external_ecu(void) {
    OcScenario s;
    scenario(&s, 1500.0, 0);
    oc_l2_loop_init_external(&M, &OC_L2_LOOP_D936LA6, &s);
    OtkCanFrame tx[4];
    unsigned tsc1 = 0;
    while (M.t_ms < 2000U) {
        const size_t n = oc_l2_loop_step(&M, tx, 4U);
        for (size_t i = 0; i < n; i++) {
            J1939Frame f;
            CHECK(oc_frame_from_otk(&tx[i], &f));
            tsc1 += j1939_id_pgn(f.id) == J1939_PGN_TSC1;
        }
    }
    CHECK(tsc1 > 100u);                                  /* one every 10 ms from the controller */
    CHECK(M.teeth_seen == 0u && M.wheel.st.teeth == 0u); /* no ECU in memory */
    CHECK(!M.ecu_eec1_seen && M.plant.plant.rpm > 50.0);  /* the starter turns it */
    /* a tick in the last 200 steps (240 degrees or more) at which the plant's angle was some cylinder's
     * TDC - 10 degrees: the start of that cylinder's injection */
    const uint32_t k0 = M.plant.step - 230U;
    double best = 1e9;
    uint64_t at = 0U;
    unsigned cyl = 0U;
    for (uint64_t t = (uint64_t)k0 * OC_L2_STEP_TICKS; t < (uint64_t)(k0 + 200U) * OC_L2_STEP_TICKS; t += 9U) {
        double u = 0.0;
        if (!oc_l2_angle_at(&M.plant, t, &u)) {
            continue;
        }
        for (unsigned c = 1U; c <= 6U; c++) {
            const double e = fabs(fmod(u - (oc_l2_tdc_deg(c) - 10.0) + 1080.0, 720.0) - 360.0);
            if (e < best) {
                best = e;
                at = t;
                cyl = c;
            }
        }
    }
    CHECK(best < 0.01 && cyl >= 1U);
    const uint32_t lag = M.plant.step - (uint32_t)(at / OC_L2_STEP_TICKS);
    CHECK(oc_l2_loop_capture(&M, cyl - 1U, at, 14040U)); /* 520 us, on that cylinder's channel */
    CHECK(M.capture_lag_max == lag);
    (void)oc_l2_loop_step(&M, tx, 4U);
    CHECK(M.plant.injections == 1u && M.plant.misplaced == 0u);
    CHECK(M.last.valid && fabs(M.last.soi_err_deg) < 0.02);
    CHECK(!M.width_measured); /* the commanded width is not known with an external ECU */
    /* an EEC1 from the engine's address */
    const J1939Eec1 e = { .torque_mode = 0, .engine_speed_valid = true, .engine_speed_rpm = 1234.0f,
                          .controlling_address = 0x27 };
    J1939Frame f;
    j1939_encode_eec1(&e, 0x00U, &f);
    OtkCanFrame o;
    oc_frame_to_otk(&f, &o);
    oc_l2_loop_rx(&M, &o);
    CHECK(M.ecu_eec1_seen && fabs(M.ecu_eec1_rpm - 1234.0) < 0.2);
}

int main(void) {
    RUN_TEST(test_edges_at_the_plants_angle);
    RUN_TEST(test_hsi_bias_in_the_ecus_speed);
    RUN_TEST(test_injection_end_to_end);
    RUN_TEST(test_lost_tooth);
    RUN_TEST(test_observables);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_external_ecu);
    return check_report();
}
