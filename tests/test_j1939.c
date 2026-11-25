/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_j1939.c
 * @brief J1939 codec and bench messages: identifiers, known vectors, round
 *        trips, saturation, "not available", short and foreign frames.
 */

#include <string.h>
#include <math.h>

#include "check.h"
#include <opencrank/j1939.h>
#include <opencrank/j1939_diag.h>
#include <opencrank/j1939_diag_tx.h>
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
    const J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f, 0U, false };
    J1939Frame f;
    j1939_encode_tsc1(&m, 0x27, 0x00, &f);
    CHECK(f.id == 0x0C000027UL);
    CHECK(f.dlc == 8U);
    CHECK(f.data[0] == 0xF1U); /* mode 01, conditions 00, priority 11, bits 6-7 not defined (1) */
    CHECK(f.data[1] == 0xE0U && f.data[2] == 0x2EU); /* 1500 / 0.125 = 12000 = 0x2EE0 */
    CHECK(f.data[3] == 0xFFU);                      /* torque not available */
    CHECK(all_ff(&f, 4, 6));
    /* Checksum by hand: F1+E0+2E+4*FF + counter 0 + id bytes 27+00+00+0C =
     * 1582 = 0x62E; 0x62 + 0x62E = 0x690; & 0x0F = 0. Byte 7 = 0 << 4 | 0. */
    CHECK(f.data[7] == 0x00U);

    J1939Tsc1 d;
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_SPEED);
    CHECK(d.mode_priority == 3U);
    CHECK(d.requested_speed_valid);
    CHECK_NEAR(d.requested_speed_rpm, 1500.0, 0.0);
    CHECK(!d.requested_torque_valid);
    CHECK(d.message_counter == 0U);
    CHECK(d.checksum_valid);
}

/* SR-10 */
static void test_tsc1_counter_and_checksum(void) {
    const J1939Tsc1 off = { J1939_OVERRIDE_DISABLED, 0, 3, false, 0.0f, false, 0.0f, 15U, false };
    J1939Frame f;
    J1939Tsc1 d;
    j1939_encode_tsc1(&off, 0x27, 0x00, &f);
    /* By hand: F0+6*FF + counter 15 + 27+0C = 1836 = 0x72C; 0x72 + 0x72C =
     * 0x79E; & 0x0F = 0xE. Byte 7 = 0xE << 4 | 0xF. */
    CHECK(f.data[7] == 0xEFU);
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.message_counter == 15U && d.checksum_valid);

    /* Every counter value round-trips with a valid checksum; only bits 0-3 count. */
    for (uint8_t c = 0U; c < 32U; c++) {
        J1939Tsc1 m = off;
        m.message_counter = c;
        j1939_encode_tsc1(&m, 0x27, 0x00, &f);
        CHECK(j1939_decode_tsc1(&f, &d));
        CHECK(d.message_counter == (uint8_t)(c & 0x0FU));
        CHECK(d.checksum_valid);
    }

    /* Any flipped data byte, a different counter or a different sender is caught. */
    const J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f, 5U, false };
    for (size_t i = 0U; i < 7U; i++) {
        j1939_encode_tsc1(&m, 0x27, 0x00, &f);
        f.data[i] ^= 0xFFU;
        CHECK(j1939_decode_tsc1(&f, &d));
        CHECK(!d.checksum_valid);
    }
    j1939_encode_tsc1(&m, 0x27, 0x00, &f);
    f.data[7] = (uint8_t)((f.data[7] & 0xF0U) | 6U);
    CHECK(j1939_decode_tsc1(&f, &d) && !d.checksum_valid);
    j1939_encode_tsc1(&m, 0x27, 0x00, &f);
    f.id = j1939_make_id(3, J1939_PGN_TSC1, 0x00, 0x28);
    CHECK(j1939_decode_tsc1(&f, &d) && !d.checksum_valid);
}

static void test_tsc1_round_trip_and_disabled(void) {
    const J1939Tsc1 m = { J1939_OVERRIDE_TORQUE, 2, 1, true, 987.375f, true, 50.0f, 7U, false };
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

    const J1939Tsc1 off = { J1939_OVERRIDE_DISABLED, 0, 3, false, 0.0f, false, 0.0f, 15U, false };
    j1939_encode_tsc1(&off, 0x27, 0x00, &f);
    CHECK(f.data[0] == 0xF0U);
    CHECK(all_ff(&f, 1, 6));
    CHECK(j1939_decode_tsc1(&f, &d));
    CHECK(d.override_mode == J1939_OVERRIDE_DISABLED);
    CHECK(!d.requested_speed_valid);

    const J1939Tsc1 s = { J1939_OVERRIDE_SPEED_TORQUE_LIMIT, 2, 1, true, 987.375f, true, 50.0f, 3U, false };
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

/* SR-12, SR-13 */
/* The engine's lamps and codes are read exactly. */
static void test_dm1_known_vectors(void) {
    /* One code, red stop lamp: SPN 100 FMI 1, count 1. By hand: lamps
     * 0b00010000; SPN 100 = 0x64 0x00, high bits 0 << 5 | FMI 1; count 1. */
    J1939Dm1 m = { 0 };
    m.red_stop = J1939_LAMP_ON;
    m.n_dtc = 1U;
    m.dtc[0] = (J1939Dtc){ 100U, 1U, 1U };
    uint8_t d[J1939_DM1_MAX_LEN];
    CHECK(j1939_encode_dm1(&m, d, sizeof(d)) == 8U);
    const uint8_t want[8] = { 0x10U, 0xFFU, 0x64U, 0x00U, 0x01U, 0x01U, 0xFFU, 0xFFU };
    CHECK(memcmp(d, want, 8) == 0);
    J1939Dm1 r;
    CHECK(j1939_decode_dm1(d, 8U, &r));
    CHECK(r.red_stop == J1939_LAMP_ON && r.amber_warning == J1939_LAMP_OFF && r.mil == J1939_LAMP_OFF);
    CHECK(r.n_dtc == 1U && r.dtc[0].spn == 100U && r.dtc[0].fmi == 1U && r.dtc[0].oc == 1U);

    /* No active code: SPN 0 FMI 0 is no DTC. */
    J1939Dm1 none = { 0 };
    CHECK(j1939_encode_dm1(&none, d, sizeof(d)) == 8U);
    CHECK(d[0] == 0x00U && d[2] == 0U && d[3] == 0U && d[4] == 0U && d[5] == 0U);
    CHECK(j1939_decode_dm1(d, 8U, &r) && r.n_dtc == 0U && r.red_stop == J1939_LAMP_OFF);

    /* The SPN's three high bits: SPN 0x5A5A5, FMI 9 -> A5 A5, 101 << 5 | 01001 = 0xA9. */
    m.dtc[0] = (J1939Dtc){ 0x5A5A5U, 9U, 3U };
    m.red_stop = J1939_LAMP_OFF;
    m.amber_warning = J1939_LAMP_ON;
    CHECK(j1939_encode_dm1(&m, d, sizeof(d)) == 8U);
    CHECK(d[0] == 0x04U && d[2] == 0xA5U && d[3] == 0xA5U && d[4] == 0xA9U && d[5] == 0x03U);
    CHECK(j1939_decode_dm1(d, 8U, &r) && r.dtc[0].spn == 0x5A5A5U && r.dtc[0].fmi == 9U);
    CHECK(r.amber_warning == J1939_LAMP_ON && r.red_stop == J1939_LAMP_OFF);

    /* Two codes: 10 bytes, no padding. Too short to decode: false. */
    m.n_dtc = 2U;
    m.dtc[1] = (J1939Dtc){ 110U, 16U, 1U };
    CHECK(j1939_encode_dm1(&m, d, sizeof(d)) == 10U);
    CHECK(j1939_decode_dm1(d, 10U, &r) && r.n_dtc == 2U && r.dtc[1].spn == 110U && r.dtc[1].fmi == 16U);
    CHECK(!j1939_decode_dm1(d, 5U, &r));
    CHECK(j1939_encode_dm1(&m, d, 9U) == 0U); /* no room */
}

/* SR-12 */
/* A two-code DM1 crosses as a BAM, and a broken BAM is dropped. */
static void test_bam(void) {
    uint8_t msg[10];
    for (uint8_t i = 0U; i < 10U; i++) {
        msg[i] = (uint8_t)(0x10U + i);
    }
    J1939Frame fr[J1939_TP_MAX_PACKETS + 1U];
    CHECK(j1939_bam_encode(J1939_PGN_DM1, msg, 10U, 0x00U, fr, 8U) == 3U);
    /* TP.CM: priority 6, PGN 0xEC00 to 0xFF, from 0x00; BAM, 10 bytes, 2 packets, PGN 0x00FECA. */
    CHECK(fr[0].id == 0x18ECFF00UL);
    CHECK(fr[0].data[0] == 32U && fr[0].data[1] == 10U && fr[0].data[2] == 0U && fr[0].data[3] == 2U);
    CHECK(fr[0].data[4] == 0xFFU && fr[0].data[5] == 0xCAU && fr[0].data[6] == 0xFEU && fr[0].data[7] == 0x00U);
    CHECK(fr[1].id == 0x18EBFF00UL && fr[1].data[0] == 1U && fr[1].data[1] == 0x10U && fr[1].data[7] == 0x16U);
    CHECK(fr[2].data[0] == 2U && fr[2].data[1] == 0x17U && fr[2].data[3] == 0x19U && fr[2].data[4] == 0xFFU);
    CHECK(j1939_bam_encode(J1939_PGN_DM1, msg, 8U, 0x00U, fr, 8U) == 0U); /* fits in one frame */
    CHECK(j1939_bam_encode(J1939_PGN_DM1, msg, 10U, 0x00U, fr, 2U) == 0U); /* no room */

    J1939BamRx rx;
    uint32_t pgn = 0U;
    j1939_bam_rx_init(&rx);
    CHECK(!j1939_bam_rx_frame(&rx, &fr[0], 1000U, &pgn));
    CHECK(!j1939_bam_rx_frame(&rx, &fr[1], 1050U, &pgn));
    CHECK(j1939_bam_rx_frame(&rx, &fr[2], 1100U, &pgn));
    CHECK(pgn == J1939_PGN_DM1 && rx.size == 10U && memcmp(rx.buf, msg, 10) == 0);

    /* Out of sequence: dropped, and the rest of it is ignored. */
    (void)j1939_bam_rx_frame(&rx, &fr[0], 2000U, &pgn);
    CHECK(!j1939_bam_rx_frame(&rx, &fr[2], 2050U, &pgn));
    CHECK(!rx.active);
    CHECK(!j1939_bam_rx_frame(&rx, &fr[1], 2100U, &pgn));

    /* A packet later than T1 (750 ms): dropped. */
    (void)j1939_bam_rx_frame(&rx, &fr[0], 3000U, &pgn);
    (void)j1939_bam_rx_frame(&rx, &fr[1], 3050U, &pgn);
    CHECK(!j1939_bam_rx_frame(&rx, &fr[2], 3050U + J1939_TP_T1_MS + 1U, &pgn));
    CHECK(!rx.active);

    /* A new TP.CM restarts: the message completes from the new one. */
    (void)j1939_bam_rx_frame(&rx, &fr[0], 4000U, &pgn);
    (void)j1939_bam_rx_frame(&rx, &fr[1], 4050U, &pgn);
    (void)j1939_bam_rx_frame(&rx, &fr[0], 4100U, &pgn);
    CHECK(!j1939_bam_rx_frame(&rx, &fr[2], 4150U, &pgn)); /* expects packet 1 again */
    (void)j1939_bam_rx_frame(&rx, &fr[0], 4200U, &pgn);
    (void)j1939_bam_rx_frame(&rx, &fr[1], 4250U, &pgn);
    CHECK(j1939_bam_rx_frame(&rx, &fr[2], 4300U, &pgn));

    /* An announcement that does not add up (3 packets for 10 bytes) is ignored. */
    fr[0].data[3] = 3U;
    (void)j1939_bam_rx_frame(&rx, &fr[0], 5000U, &pgn);
    CHECK(!rx.active);
}

int main(void) {
    RUN_TEST(test_identifiers);
    RUN_TEST(test_tsc1_known_vector);
    RUN_TEST(test_tsc1_counter_and_checksum);
    RUN_TEST(test_tsc1_round_trip_and_disabled);
    RUN_TEST(test_eec1_known_vector);
    RUN_TEST(test_saturation);
    RUN_TEST(test_rounding);
    RUN_TEST(test_not_available_decodes_invalid);
    RUN_TEST(test_rejects_short_and_foreign_frames);
    RUN_TEST(test_et1_eflp1_vectors);
    RUN_TEST(test_bench_messages);
    RUN_TEST(test_dm1_known_vectors);
    RUN_TEST(test_bam);
    return check_report();
}
