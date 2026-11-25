/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_bench.c
 * @brief The simulated machine on the bus: message periods, TSC1 reaching
 *        the engine ECU, pump command timeout, fault injection, links.
 */

#include "check.h"
#include <opencrank/oc_msgs.h>
#include <opencrank/runtime/bench.h>

typedef struct {
    unsigned eec1, et1, eflp1, op;
} Counts;

static void count(const J1939Frame *tx, size_t n, Counts *c) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t pgn = j1939_id_pgn(tx[i].id);
        c->eec1 += (pgn == J1939_PGN_EEC1);
        c->et1 += (pgn == J1939_PGN_ET1);
        c->eflp1 += (pgn == J1939_PGN_EFLP1);
        c->op += (pgn == OC_PGN_OPERATOR);
    }
}

static Counts run(OcBench *b, uint32_t ms) {
    Counts c = { 0, 0, 0, 0 };
    J1939Frame tx[8];
    for (uint32_t t = 0; t < ms; t += b->cfg->step_ms) {
        count(tx, oc_bench_step(b, tx, 8), &c);
    }
    return c;
}

static uint8_t tsc1_counter; /* SPN 4206: the bench refuses a repeated counter */

static void send_tsc1(OcBench *b, J1939OverrideMode mode, float rpm) {
    const J1939Tsc1 m = { mode, 0, 3, mode == J1939_OVERRIDE_SPEED, rpm, false, 0.0f, tsc1_counter, false };
    tsc1_counter = (uint8_t)((tsc1_counter + 1U) & 0x0FU);
    J1939Frame f;
    j1939_encode_tsc1(&m, OC_ADDR_MACHINE_CTRL, J1939_ADDR_ENGINE_1, &f);
    oc_bench_rx(b, &f);
}

static void start_engine(OcBench *b) {
    b->sig.key_on = true;
    b->sig.start = true;
    (void)run(b, 3000U);
    b->sig.start = false;
    (void)run(b, 3000U);
}

static void test_message_periods(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    b.sig.key_on = true;
    const Counts c = run(&b, 1000U);
    CHECK(c.eec1 == 100U);
    CHECK(c.eflp1 == 2U);
    CHECK(c.et1 == 1U);
    CHECK(c.op == 50U);
}

static void test_engine_silent_when_key_off(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    const Counts c = run(&b, 1000U);
    CHECK(c.eec1 == 0U && c.et1 == 0U && c.eflp1 == 0U);
    CHECK(c.op == 50U); /* the operator panel still talks */

    b.sig.key_on = true;
    b.sig.fault_engine_tx_lost = true;
    const Counts d = run(&b, 1000U);
    CHECK(d.eec1 == 0U);
}

static void test_tsc1_reaches_engine_ecu(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    CHECK(b.ecu.state == ECU_STATE_RUNNING);
    for (int i = 0; i < 500; i++) {
        send_tsc1(&b, J1939_OVERRIDE_SPEED, 1500.0f);
        (void)run(&b, 10U);
    }
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    CHECK(b.ecu.controlling_address == OC_ADDR_MACHINE_CTRL);
    CHECK_NEAR(b.plant.rpm, 1500.0, 5.0);

    /* A TSC1 addressed to another engine is not ours. */
    OcBench c;
    oc_bench_init(&c, &OC_BENCH_D936LA6);
    start_engine(&c);
    const J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f, 0U, false };
    J1939Frame f;
    j1939_encode_tsc1(&m, OC_ADDR_MACHINE_CTRL, 0x01, &f);
    oc_bench_rx(&c, &f);
    (void)run(&c, 10U);
    CHECK(c.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
}

static void test_mc_frames_lost(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    b.sig.fault_mc_tx_lost = true;
    send_tsc1(&b, J1939_OVERRIDE_SPEED, 1500.0f);
    (void)run(&b, 10U);
    CHECK(b.rx_dropped == 1U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
}

/* The engine link delays the engine's frames, not the operator panel's. */
static void test_engine_link_delay(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    b.sig.link_engine.delay = otk_ns_from_ms(100);
    Counts c = run(&b, 90U);
    CHECK(c.eec1 == 0U); /* every EEC1 sent so far is still in flight */
    CHECK(c.op > 0U);
    c = run(&b, 200U);
    CHECK(c.eec1 == 19U); /* then one every 10 ms again, 100 ms old: the first is due 110 ms after the change */
    CHECK(b.engine_link.overflows == 0U);
}

/* The mc link delays the controller's frames on their way to the ECU. */
static void test_mc_link_delay(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    b.sig.link_mc.delay = otk_ns_from_ms(50);
    (void)run(&b, 10U); /* the link takes the new value at the start of a step */
    send_tsc1(&b, J1939_OVERRIDE_SPEED, 1500.0f);
    (void)run(&b, 30U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    (void)run(&b, 30U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_REQUEST);
    CHECK(b.rx_frames == 1U);
}

/* Bus-off in the mc direction: nothing the controller sends arrives. */
static void test_mc_link_bus_off(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    b.sig.link_mc.bus_off = true;
    (void)run(&b, 10U);
    send_tsc1(&b, J1939_OVERRIDE_SPEED, 1500.0f);
    (void)run(&b, 10U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);
    CHECK(b.mc_link.bus_off_lost == 1U);
    CHECK(b.rx_frames == 0U);
}

/* SR-10: the ECU acts only on a TSC1 whose checksum is right and whose counter moved. */
static void test_tsc1_integrity_checked(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    J1939Frame f;
    const J1939Tsc1 m = { J1939_OVERRIDE_SPEED, 0, 3, true, 1500.0f, false, 0.0f, 4U, false };

    j1939_encode_tsc1(&m, OC_ADDR_MACHINE_CTRL, J1939_ADDR_ENGINE_1, &f);
    f.data[2] ^= 0xFFU; /* the speed's high byte: 1500 rpm would read as 6716 */
    oc_bench_rx(&b, &f);
    (void)run(&b, 10U);
    CHECK(b.tsc1_rejected == 1U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_LOW_IDLE);

    j1939_encode_tsc1(&m, OC_ADDR_MACHINE_CTRL, J1939_ADDR_ENGINE_1, &f);
    oc_bench_rx(&b, &f); /* intact: accepted */
    oc_bench_rx(&b, &f); /* same counter again: refused */
    (void)run(&b, 10U);
    CHECK(b.tsc1_rejected == 2U);
    CHECK(b.rx_frames == 1U);
    CHECK(b.ecu.source == ECU_SPEED_SOURCE_REQUEST);
}

static void test_pump_command_times_out(void) {
    OcBench b;
    oc_bench_init(&b, &OC_BENCH_D936LA6);
    start_engine(&b);
    const OcPumpCmd cmd = { true, 50.0f };
    J1939Frame f;
    oc_encode_pump_cmd(&cmd, OC_ADDR_MACHINE_CTRL, &f);
    oc_bench_rx(&b, &f);
    (void)run(&b, 100U);
    CHECK_NEAR(b.pump_cmd_applied_pct, 50.0, 0.0);
    (void)run(&b, b.cfg->pump_cmd_timeout_ms + 20U);
    CHECK(b.pump_cmd_applied_pct == 0.0);
}

int main(void) {
    RUN_TEST(test_message_periods);
    RUN_TEST(test_engine_silent_when_key_off);
    RUN_TEST(test_tsc1_reaches_engine_ecu);
    RUN_TEST(test_mc_frames_lost);
    RUN_TEST(test_pump_command_times_out);
    RUN_TEST(test_tsc1_integrity_checked);
    RUN_TEST(test_engine_link_delay);
    RUN_TEST(test_mc_link_delay);
    RUN_TEST(test_mc_link_bus_off);
    return check_report();
}
