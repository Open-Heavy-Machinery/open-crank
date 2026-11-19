/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_j1939.c
 * @brief J1939 codec and bench messages: identifiers, known vectors, round
 *        trips, saturation, "not available", short and foreign frames.
 */

#include <math.h>

#include "check.h"
#include <opencrank/j1939.h>
#include <opencrank/oc_msgs.h>

static int all_ff(const J1939Frame *f, int from, int to) {
    for (int i = from; i <= to; i++) {
        if (f->data[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

static void test_identifiers(void) {
    CHECK(j1939_make_id(3, J1939_PGN_EEC1, 0x55, 0x00) == 0x0CF00400UL); /* PDU2 ignores dest */
    CHECK(j1939_make_id(3, J1939_PGN_TSC1, 0x00, 0x27) == 0x0C000027UL);
    CHECK(j1939_make_id(6, J1939_PGN_ET1, 0xFF, 0x00) == 0x18FEEE00UL);
    CHECK(j1939_make_id(6, J1939_PGN_EFLP1, 0xFF, 0x00) == 0x18FEEF00UL);

    CHECK(j1939_id_pgn(0x0C000027UL) == J1939_PGN_TSC1);
    CHECK(j1939_id_dest(0x0C000027UL) == 0x00U);
    CHECK(j1939_id_source(0x0C000027UL) == 0x27U);
    CHECK(j1939_id_priority(0x0C000027UL) == 3U);
    CHECK(j1939_id_pgn(0x0C001127UL) == J1939_PGN_TSC1); /* TSC1 to another address */
    CHECK(j1939_id_dest(0x0C001127UL) == 0x11U);
    CHECK(j1939_id_pgn(0x18FEEF00UL) == J1939_PGN_EFLP1);
    CHECK(j1939_id_dest(0x18FEEF00UL) == J1939_ADDR_GLOBAL);
    CHECK(j1939_id_pgn(0x18F00400UL) == J1939_PGN_EEC1); /* priority does not matter */
}

static void test_tsc1_known_vector(void) {
    const J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f };
    J1939Frame f;
    j1939_encode_tsc1(&m, 0x27, 0x00, &f);
    CHECK(f.id == 0x0C000027UL);
    CHECK(f.dlc == 8U);
    CHECK(f.data[0] == 0xF1U); /* mode 01, conditions 00, priority 11, bits 6-7 not defined (1) */
    CHECK(f.data[1] == 0xE0U && f.data[2] == 0x2EU); /* 1500 / 0.125 = 12000 = 0x2EE0 */
    CHECK(f.data[3] == 0xFFU);                      /* torque not available */
    CHECK(all_ff(&f, 4, 7));                         /* counter / checksum not available */

    J1939Tsc1 d;
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_SPEED);
    CHECK(d.mode_priority == 3U);
    CHECK(d.requested_speed_valid);
    CHECK_NEAR(d.requested_speed_rpm, 1500.0, 0.0);
    CHECK(!d.requested_torque_valid);
}

static void test_tsc1_round_trip_and_disabled(void) {
    const J1939Tsc1 m = { J1939_OVERRIDE_TORQUE, 2, 1, true, 987.375f, true, 50.0f };
    J1939Frame f;
    J1939Tsc1 d;
    j1939_encode_tsc1(&m, 0x27, 0x00, &f);
    CHECK(f.data[3] == 175U);
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_TORQUE);
    CHECK(d.speed_conditions == 2U);
    CHECK(d.mode_priority == 1U);
    CHECK_NEAR(d.requested_speed_rpm, 987.375, 0.0);
    CHECK_NEAR(d.requested_torque_pct, 50.0, 0.0);

    const J1939Tsc1 off = { J1939_OVERRIDE_DISABLED, 0, 3, false, 0.0f, false, 0.0f };
    j1939_encode_tsc1(&off, 0x27, 0x00, &f);
    CHECK(f.data[0] == 0xF0U);
    CHECK(all_ff(&f, 1, 7));
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_DISABLED);
    CHECK(!d.requested_speed_valid);

    const J1939Tsc1 s = { J1939_OVERRIDE_SPEED_TORQUE_LIMIT, 2, 1, true, 987.375f, true, 50.0f };
    j1939_encode_tsc1(&s, 0x27, 0x00, &f);
    CHECK((f.data[0] & 0x03U) == 0x03U);   
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_SPEED_TORQUE_LIMIT);
    CHECK(d.speed_conditions == 2U);
    CHECK(d.mode_priority == 1U);
}

static void test_eec1_known_vector(void) {
    const J1939Eec1 m = { J1939_TORQUE_MODE_LOW_IDLE, true, 45.0f, true, 40.0f, true, 1500.0f, 0x27 };
    J1939Frame f;
    j1939_encode_eec1(&m, 0x00, &f);
    CHECK(f.id == 0x0CF00400UL);
    CHECK(f.data[0] == 0xF0U);
    CHECK(f.data[1] == 170U); /* 45 % + 125 */
    CHECK(f.data[2] == 165U); /* 40 % + 125 */
    CHECK(f.data[3] == 0xE0U && f.data[4] == 0x2EU);
    CHECK(f.data[5] == 0x27U);
    CHECK(all_ff(&f, 6, 7));

    J1939Eec1 d;
    CHECK(j1939_decode_eec1(&f, &d));
    CHECK(d.torque_mode == J1939_TORQUE_MODE_LOW_IDLE);
    CHECK_NEAR(d.engine_speed_rpm, 1500.0, 0.0);
    CHECK_NEAR(d.actual_torque_pct, 40.0, 0.0);
    CHECK_NEAR(d.driver_demand_torque_pct, 45.0, 0.0);
    CHECK(d.controlling_address == 0x27U);
}

/* Out-of-range values clamp; they never wrap or hit the error range. */
static void test_saturation(void) {
    J1939Frame f;
    J1939Eec1 m = { 0, true, 200.0f, true, -300.0f, true, 9000.0f, 0x00 };
    j1939_encode_eec1(&m, 0x00, &f);
    CHECK(f.data[3] == 0xFFU && f.data[4] == 0xFAU); /* 0xFAFF: 8031.875 rpm, top of the valid range */
    CHECK(f.data[1] == 250U);                       /* +125 % */
    CHECK(f.data[2] == 0U);                         /* -125 % */

    m.engine_speed_rpm = -50.0f;
    j1939_encode_eec1(&m, 0x00, &f);
    CHECK(f.data[3] == 0U && f.data[4] == 0U);

    m.engine_speed_rpm = NAN;
    j1939_encode_eec1(&m, 0x00, &f);
    CHECK(f.data[3] == 0U && f.data[4] == 0U);

    J1939Et1 t = { true, 300.0f };
    j1939_encode_et1(&t, 0x00, &f);
    CHECK(f.data[0] == 250U);
    t.coolant_temp_c = -60.0f;
    j1939_encode_et1(&t, 0x00, &f);
    CHECK(f.data[0] == 0U);
}

static void test_rounding(void) {
    J1939Frame f;
    const J1939Eec1 m = { 0, false, 0.0f, false, 0.0f, true, 1500.06f, 0x00 };
    j1939_encode_eec1(&m, 0x00, &f);
    CHECK(f.data[3] == 0xE0U && f.data[4] == 0x2EU);

    const J1939Eflp1 oil = { true, 449.0f }; /* 112.25 -> 112 */
    j1939_encode_eflp1(&oil, 0x00, &f);
    CHECK(f.data[3] == 112U);
}

static void test_not_available_decodes_invalid(void) {
    J1939Frame f;
    J1939Eec1 d;
    const J1939Eec1 m = { 0, true, 0.0f, true, 0.0f, true, 1000.0f, 0x00 };
    j1939_encode_eec1(&m, 0x00, &f);

    f.data[3] = 0xFF;
    f.data[4] = 0xFF;
    CHECK(j1939_decode_eec1(&f, &d));
    CHECK(!d.engine_speed_valid);

    f.data[3] = 0x00;
    f.data[4] = 0xFB; /* error range */
    CHECK(j1939_decode_eec1(&f, &d));
    CHECK(!d.engine_speed_valid);

    f.data[3] = 0xFF;
    f.data[4] = 0xFA;
    CHECK(j1939_decode_eec1(&f, &d));
    CHECK(d.engine_speed_valid);
    CHECK_NEAR(d.engine_speed_rpm, 8031.875, 0.0);

    f.data[2] = 0xFE; /* error indicator */
    CHECK(j1939_decode_eec1(&f, &d));
    CHECK(!d.actual_torque_valid);

    J1939Frame e;
    J1939Et1 et;
    const J1939Et1 unknown = { false, 0.0f };
    j1939_encode_et1(&unknown, 0x00, &e);
    CHECK(j1939_decode_et1(&e, &et));
    CHECK(!et.coolant_temp_valid);
}

static void test_rejects_short_and_foreign_frames(void) {
    J1939Frame f;
    J1939Eec1 d;
    J1939Tsc1 t;
    const J1939Eec1 m = { 0, true, 0.0f, true, 0.0f, true, 1000.0f, 0x00 };
    j1939_encode_eec1(&m, 0x00, &f);

    CHECK(!j1939_decode_tsc1(&f, &t)); /* wrong PGN */
    f.dlc = 7U;
    CHECK(!j1939_decode_eec1(&f, &d)); /* short */
    f.dlc = 0U;
    CHECK(!j1939_decode_eec1(&f, &d));

    J1939Frame o;
    J1939Eflp1 oil;
    const J1939Et1 t1 = { true, 80.0f };
    j1939_encode_et1(&t1, 0x00, &o);
    CHECK(!j1939_decode_eflp1(&o, &oil));
}

static void test_et1_eflp1_vectors(void) {
    J1939Frame f;
    const J1939Et1 t = { true, 90.0f };
    j1939_encode_et1(&t, 0x00, &f);
    CHECK(f.id == 0x18FEEE00UL);
    CHECK(f.data[0] == 130U);
    CHECK(all_ff(&f, 1, 7));
    J1939Et1 td;
    CHECK(j1939_decode_et1(&f, &td));
    CHECK_NEAR(td.coolant_temp_c, 90.0, 0.0);

    const J1939Eflp1 oil = { true, 452.0f };
    j1939_encode_eflp1(&oil, 0x00, &f);
    CHECK(f.id == 0x18FEEF00UL);
    CHECK(f.data[3] == 113U);
    CHECK(all_ff(&f, 0, 2) && all_ff(&f, 4, 7));
    J1939Eflp1 od;
    CHECK(j1939_decode_eflp1(&f, &od));
    CHECK_NEAR(od.oil_pressure_kpa, 452.0, 0.0);
}

static void test_bench_messages(void) {
    J1939Frame f;
    const OcOperator op = { true, 1500.0f, true, 85.2f, true };
    oc_encode_operator(&op, OC_ADDR_BENCH, &f);
    CHECK(j1939_id_pgn(f.id) == OC_PGN_OPERATOR);
    CHECK(j1939_id_source(f.id) == OC_ADDR_BENCH);
    CHECK(f.data[2] == 213U); /* 85.2 / 0.4 */
    CHECK((f.data[3] & 0x03U) == 0x01U);
    OcOperator d;
    CHECK(oc_decode_operator(&f, &d));
    CHECK(d.target_speed_valid && d.hoist_demand_valid && d.fault_reset_pressed);
    CHECK_NEAR(d.target_speed_rpm, 1500.0, 0.0);
    CHECK_NEAR(d.hoist_demand_pct, 85.2, 1e-4);

    const OcOperator none = { false, 0.0f, false, 0.0f, false };
    oc_encode_operator(&none, OC_ADDR_BENCH, &f);
    CHECK(oc_decode_operator(&f, &d));
    CHECK(!d.target_speed_valid && !d.hoist_demand_valid && !d.fault_reset_pressed);

    const OcPumpCmd pump = { true, 150.0f };
    oc_encode_pump_cmd(&pump, OC_ADDR_MACHINE_CTRL, &f);
    CHECK(j1939_id_pgn(f.id) == OC_PGN_PUMP_CMD);
    CHECK(f.data[0] == 250U); /* saturates at 100 % */
    OcPumpCmd pd;
    CHECK(oc_decode_pump_cmd(&f, &pd));
    CHECK_NEAR(pd.displacement_pct, 100.0, 0.0);
    CHECK(!oc_decode_operator(&f, &d));
}

int main(void) {
    RUN_TEST(test_identifiers);
    RUN_TEST(test_tsc1_known_vector);
    RUN_TEST(test_tsc1_round_trip_and_disabled);
    RUN_TEST(test_eec1_known_vector);
    RUN_TEST(test_saturation);
    RUN_TEST(test_rounding);
    RUN_TEST(test_not_available_decodes_invalid);
    RUN_TEST(test_rejects_short_and_foreign_frames);
    RUN_TEST(test_et1_eflp1_vectors);
    RUN_TEST(test_bench_messages);
    return check_report();
}
