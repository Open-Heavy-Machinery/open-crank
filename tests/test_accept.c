/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_accept.c
 * @brief open-crank's observables for the acceptance oracle (sim/accept):
 *        decoding of TSC1, OC_PUMP_CMD and the DUT's DM1, defaults, and frames from other
 *        nodes. The oracle itself is tested in open-tick.
 */

#include <string.h>

#include "check.h"
#include <opencrank/accept.h>
#include <opencrank/can_otk.h>
#include <opencrank/j1939.h>
#include <opencrank/j1939_diag.h>
#include <opencrank/oc_msgs.h>
#include <opentick/oracle.h>

/* ------------------------------------------------------------ open-crank's table */

static OcAccept accept;
static OtkOracleTable accept_table;

static size_t decode_j1939(const J1939Frame *jf, OtkOracleUpdate *up) {
    OtkCanFrame f;
    oc_frame_to_otk(jf, &f);
    return oc_accept_decode(&accept, &f, up, OTK_ORACLE_MAX_OBSERVABLES);
}

static J1939Frame tsc1_frame(J1939OverrideMode mode, bool speed_valid, float rpm, uint8_t src, uint8_t dest) {
    const J1939Tsc1 m = {
        .override_mode = mode,
        .mode_priority = 3U, /* lowest, as in byte 0 = 0xF1 / 0xF0 */
        .requested_speed_valid = speed_valid,
        .requested_speed_rpm = rpm,
    };
    J1939Frame f;
    j1939_encode_tsc1(&m, src, dest, &f);
    return f;
}

static void test_accept_table(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    CHECK(accept_table.count == 7U);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_SPEED_CONTROL].name, "tsc1_speed_control") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_SPEED_RPM].name, "tsc1_speed_rpm") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_PUMP_CMD_PCT].name, "pump_cmd_pct") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_OVERRIDE].name, "tsc1_override") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_INTACT].name, "tsc1_intact") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_RED_STOP].name, "dm1_red_stop") == 0);
    CHECK(strcmp(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_SPN].name, "dm1_spn") == 0);
    for (size_t i = 0; i < OC_ACCEPT_DM1_RED_STOP; i++) {
        CHECK(OC_ACCEPT_OBSERVABLES[i].stale == 200U * OTK_NS_PER_MS);
    }
    CHECK(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_RED_STOP].stale == 3000U * OTK_NS_PER_MS);
    CHECK(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_SPN].stale == 3000U * OTK_NS_PER_MS);
    CHECK(!OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_RED_STOP].fallback.gap &&
          OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_RED_STOP].fallback.value == 0.0);
    CHECK(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_DM1_SPN].fallback.gap);
    CHECK(!OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_SPEED_CONTROL].fallback.gap &&
          OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_SPEED_CONTROL].fallback.value == 0.0);
    CHECK(OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_SPEED_RPM].fallback.gap);
    CHECK(!OC_ACCEPT_OBSERVABLES[OC_ACCEPT_PUMP_CMD_PCT].fallback.gap &&
          OC_ACCEPT_OBSERVABLES[OC_ACCEPT_PUMP_CMD_PCT].fallback.value == 0.0);
    CHECK(!OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_OVERRIDE].fallback.gap &&
          OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_OVERRIDE].fallback.value == 0.0);
    CHECK(!OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_INTACT].fallback.gap &&
          OC_ACCEPT_OBSERVABLES[OC_ACCEPT_TSC1_INTACT].fallback.value == 1.0); /* silence is not a broken frame */
}

/* SR-10: intact = checksum right and counter = previous + 1. */
static void test_accept_tsc1_intact(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];
    J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f, 14U, false };
    J1939Frame f;
    for (int i = 0; i < 4; i++) { /* 14, 15, 0, 1: the wrap is a step of one */
        j1939_encode_tsc1(&m, OC_ACCEPT_DUT_DEFAULT, J1939_ADDR_ENGINE_1, &f);
        CHECK(decode_j1939(&f, up) == 4U);
        CHECK(up[3].observable == OC_ACCEPT_TSC1_INTACT && up[3].value.value == 1.0);
        m.message_counter = (uint8_t)((m.message_counter + 1U) & 0x0FU);
    }
    j1939_encode_tsc1(&m, OC_ACCEPT_DUT_DEFAULT, J1939_ADDR_ENGINE_1, &f);
    f.data[1] ^= 0x01U; /* wrong checksum */
    CHECK(decode_j1939(&f, up) == 4U && up[3].value.value == 0.0);
    m.message_counter = (uint8_t)((m.message_counter + 1U) & 0x0FU);
    j1939_encode_tsc1(&m, OC_ACCEPT_DUT_DEFAULT, J1939_ADDR_ENGINE_1, &f);
    CHECK(decode_j1939(&f, up) == 4U && up[3].value.value == 1.0);
    j1939_encode_tsc1(&m, OC_ACCEPT_DUT_DEFAULT, J1939_ADDR_ENGINE_1, &f);
    CHECK(decode_j1939(&f, up) == 4U && up[3].value.value == 0.0); /* counter repeated */
}

static void test_accept_tsc1_speed_control(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];

    /* Byte 0 = 0xF1: speed control, with a speed. */
    J1939Frame f = tsc1_frame(J1939_OVERRIDE_SPEED, true, 1500.0f, 0x27U, 0x00U);
    CHECK(f.data[0] == 0xF1U);
    CHECK(decode_j1939(&f, up) == 4U);
    CHECK(up[0].observable == OC_ACCEPT_TSC1_SPEED_CONTROL && !up[0].value.gap && up[0].value.value == 1.0);
    CHECK(up[1].observable == OC_ACCEPT_TSC1_SPEED_RPM && !up[1].value.gap && up[1].value.value == 1500.0);
    CHECK(up[2].observable == OC_ACCEPT_TSC1_OVERRIDE && !up[2].value.gap && up[2].value.value == 1.0);

    /* Speed control without a valid speed is not speed control, but it is still an override. */
    f = tsc1_frame(J1939_OVERRIDE_SPEED, false, 0.0f, 0x27U, 0x00U);
    CHECK(f.data[0] == 0xF1U && f.data[1] == 0xFFU && f.data[2] == 0xFFU);
    CHECK(decode_j1939(&f, up) == 4U);
    CHECK(up[0].value.value == 0.0 && up[1].value.gap);
    CHECK(up[2].observable == OC_ACCEPT_TSC1_OVERRIDE && !up[2].value.gap && up[2].value.value == 1.0);
}

static void test_accept_tsc1_released(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];

    /* Byte 0 = 0xF0: override disabled. The speed bytes do not matter. */
    J1939Frame f = tsc1_frame(J1939_OVERRIDE_DISABLED, true, 1500.0f, 0x27U, 0x00U);
    CHECK(f.data[0] == 0xF0U);
    CHECK(decode_j1939(&f, up) == 4U);
    CHECK(up[0].observable == OC_ACCEPT_TSC1_SPEED_CONTROL && !up[0].value.gap && up[0].value.value == 0.0);
    CHECK(up[1].observable == OC_ACCEPT_TSC1_SPEED_RPM && up[1].value.gap);
    CHECK(up[2].observable == OC_ACCEPT_TSC1_OVERRIDE && !up[2].value.gap && up[2].value.value == 0.0);

    /* Torque control is not speed control either, but it is an override. */
    f = tsc1_frame(J1939_OVERRIDE_TORQUE, true, 1500.0f, 0x27U, 0x00U);
    CHECK(decode_j1939(&f, up) == 4U);
    CHECK(up[0].value.value == 0.0 && up[1].value.gap);
    CHECK(up[2].observable == OC_ACCEPT_TSC1_OVERRIDE && up[2].value.value == 1.0);

    /* So is a speed/torque limit. */
    f = tsc1_frame(J1939_OVERRIDE_SPEED_TORQUE_LIMIT, true, 1500.0f, 0x27U, 0x00U);
    CHECK(decode_j1939(&f, up) == 4U);
    CHECK(up[2].observable == OC_ACCEPT_TSC1_OVERRIDE && up[2].value.value == 1.0);
}

static void test_accept_ignores_other_nodes(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];

    /* From another source address. */
    J1939Frame f = tsc1_frame(J1939_OVERRIDE_SPEED, true, 1500.0f, 0x28U, 0x00U);
    CHECK(decode_j1939(&f, up) == 0U);
    /* To another destination, or to everyone. */
    f = tsc1_frame(J1939_OVERRIDE_SPEED, true, 1500.0f, 0x27U, 0x01U);
    CHECK(decode_j1939(&f, up) == 0U);
    f = tsc1_frame(J1939_OVERRIDE_SPEED, true, 1500.0f, 0x27U, J1939_ADDR_GLOBAL);
    CHECK(decode_j1939(&f, up) == 0U);
    /* A pump command from someone else. */
    const OcPumpCmd pump = { .displacement_valid = true, .displacement_pct = 50.0f };
    oc_encode_pump_cmd(&pump, 0x28U, &f);
    CHECK(decode_j1939(&f, up) == 0U);
    /* Another address for the device under test. */
    oc_accept_init(&accept, 0x28U, &accept_table);
    CHECK(decode_j1939(&f, up) == 1U);
    /* An 11-bit frame. */
    OtkCanFrame std;
    memset(&std, 0, sizeof(std));
    std.id = 0x27U;
    std.len = 8U;
    CHECK(oc_accept_decode(&accept, &std, up, OTK_ORACLE_MAX_OBSERVABLES) == 0U);
}

static void test_accept_pump_cmd(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];
    J1939Frame f;

    OcPumpCmd pump = { .displacement_valid = true, .displacement_pct = 85.0f };
    oc_encode_pump_cmd(&pump, 0x27U, &f);
    CHECK(decode_j1939(&f, up) == 1U);
    CHECK(up[0].observable == OC_ACCEPT_PUMP_CMD_PCT && !up[0].value.gap);
    CHECK(up[0].value.value >= 84.6 && up[0].value.value <= 85.4);

    /* "Not available" counts as 0, as the simulated pump takes it. */
    pump.displacement_valid = false;
    oc_encode_pump_cmd(&pump, 0x27U, &f);
    CHECK(f.data[0] == 0xFFU);
    CHECK(decode_j1939(&f, up) == 1U);
    CHECK(up[0].observable == OC_ACCEPT_PUMP_CMD_PCT && !up[0].value.gap && up[0].value.value == 0.0);
}

/* SR-14 */
static void test_accept_dm1(void) {
    oc_accept_init(&accept, OC_ACCEPT_DUT_DEFAULT, &accept_table);
    OtkOracleUpdate up[OTK_ORACLE_MAX_OBSERVABLES];
    J1939Dm1 m = { 0 };
    J1939Frame f;
    f.id = j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_DM1, J1939_ADDR_GLOBAL, 0x27U);
    f.dlc = 8U;

    /* No fault: lamp off, no DTC (a gap). */
    CHECK(j1939_encode_dm1(&m, f.data, 8U) == 8U);
    CHECK(decode_j1939(&f, up) == 2U);
    CHECK(up[0].observable == OC_ACCEPT_DM1_RED_STOP && !up[0].value.gap && up[0].value.value == 0.0);
    CHECK(up[1].observable == OC_ACCEPT_DM1_SPN && up[1].value.gap);

    /* In FAULT: red stop lamp and the first DTC's SPN. */
    m.red_stop = J1939_LAMP_ON;
    m.n_dtc = 1U;
    m.dtc[0] = (J1939Dtc){ 520194U, 31U, 1U };
    CHECK(j1939_encode_dm1(&m, f.data, 8U) == 8U);
    CHECK(decode_j1939(&f, up) == 2U);
    CHECK(up[0].value.value == 1.0);
    CHECK(!up[1].value.gap && up[1].value.value == 520194.0);

    /* The engine's DM1 is not the DUT's. */
    f.id = j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_DM1, J1939_ADDR_GLOBAL, J1939_ADDR_ENGINE_1);
    CHECK(decode_j1939(&f, up) == 0U);
}

int main(void) {
    RUN_TEST(test_accept_table);
    RUN_TEST(test_accept_dm1);
    RUN_TEST(test_accept_tsc1_speed_control);
    RUN_TEST(test_accept_tsc1_intact);
    RUN_TEST(test_accept_tsc1_released);
    RUN_TEST(test_accept_ignores_other_nodes);
    RUN_TEST(test_accept_pump_cmd);
    return check_report();
}
