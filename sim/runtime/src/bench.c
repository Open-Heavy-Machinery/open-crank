/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bench.c
 * @brief The simulated machine as seen from the CAN bus. See bench.h.
 */

#include <opencrank/runtime/bench.h>

#include <string.h>

#include <opencrank/can_otk.h>
#include <opencrank/oc_msgs.h>
#include <opencrank/plants/d936la6.h>

/* Bench test frame, from the bench (0xFC), PGN 0xFF2D, sent every step while
 * test.dut_hang is 1. Only a hang-test build of the board firmware reacts to
 * it (platforms/stm32g4/demo/main.c, OC_WDG_HANG_TEST): it stops kicking its
 * watchdog. Every other device under test ignores it. */
#define OC_TEST_HANG_ID 0x18FF2DFCUL

const OcBenchConfig OC_BENCH_D936LA6 = {
    .plant = &PLANT_D936LA6,
    .ecu = &ENGINE_ECU_D936LA6,
    .pump = &HYDRAULIC_PUMP_HOIST,
    .step_ms = 10U,
    .pump_cmd_timeout_ms = 200U,
    .eec1_period_ms = 10U,
    .eflp1_period_ms = 500U,
    .et1_period_ms = 1000U,
    .operator_period_ms = 20U,
};

void oc_bench_init(OcBench *b, const OcBenchConfig *cfg) {
    memset(b, 0, sizeof(*b));
    b->cfg = cfg;
    plant_init(&b->plant, cfg->plant);
    engine_ecu_init(&b->ecu, cfg->ecu);
    hydraulic_pump_init(&b->pump, cfg->pump);
    otk_link_init(&b->mc_link, OTK_CAN_QUEUE_MAX, OC_BENCH_LINK_MC_SEED);
    otk_link_init(&b->engine_link, OTK_CAN_QUEUE_MAX, OC_BENCH_LINK_ENGINE_SEED);
}

static void apply_signal(void *ctx, OtkSignalId signal, double value) {
    oc_signals_set((OcSignals *)ctx, (OcSignalId)signal, value);
}

void oc_bench_set_scenario(OcBench *b, const OcScenario *s) {
    otk_player_init(&b->player, s, apply_signal, &b->sig);
}

bool oc_bench_scenario_done(const OcBench *b) {
    return otk_player_done(&b->player, otk_ns_from_ms(b->t_ms));
}

static void apply_due_events(OcBench *b) {
    otk_player_advance(&b->player, otk_ns_from_ms(b->t_ms));
    otk_link_set_faults(&b->mc_link, &b->sig.link_mc);
    otk_link_set_faults(&b->engine_link, &b->sig.link_engine);
}

uint32_t oc_bench_link_overflows(const OcBench *b) {
    return b->mc_link.overflows + b->engine_link.overflows;
}

double oc_bench_time_s(const OcBench *b) {
    return (double)b->t_ms / 1000.0;
}

/* A frame from the device under test, as the machine receives it. */
static void apply_rx(OcBench *b, const J1939Frame *f) {
    J1939Tsc1 tsc1;
    OcPumpCmd pump;
    const uint8_t dest = j1939_id_dest(f->id);
    const uint8_t ecu_addr = b->cfg->ecu->source_address;

    if ((dest == ecu_addr || dest == J1939_ADDR_GLOBAL) && j1939_decode_tsc1(f, &tsc1)) {
        if (!b->sig.key_on) {
            return; /* an unpowered ECU hears nothing */
        }
        /* SPN 4206/4207: a TSC1 whose checksum is wrong, or whose counter has
         * not moved since the last one accepted, is not a request. To the ECU
         * it is a lost frame: the last good request ages towards its timeout. */
        const bool repeated = b->tsc1_counter_seen && tsc1.message_counter == b->tsc1_counter;
        if (!tsc1.checksum_valid || repeated) {
            b->tsc1_rejected++;
            return;
        }
        b->tsc1_counter_seen = true;
        b->tsc1_counter = tsc1.message_counter;
        const EngineEcuRequest req = {
            .mode = (EngineEcuOverride)tsc1.override_mode,
            .speed_valid = tsc1.requested_speed_valid,
            .speed_rpm = (double)tsc1.requested_speed_rpm,
            .source_address = j1939_id_source(f->id),
        };
        engine_ecu_request(&b->ecu, &req);
        b->rx_frames++;
    } else if (oc_decode_pump_cmd(f, &pump)) {
        b->pump_cmd_pct = pump.displacement_valid ? (double)pump.displacement_pct : 0.0;
        b->pump_cmd_seen = true;
        b->pump_cmd_ms = b->t_ms;
        b->rx_frames++;
    } else {
        /* not addressed to the simulated machine */
    }
}

/* Applies the frames of the mc link that are due now. */
static void deliver_mc(OcBench *b) {
    OtkCanFrame o;
    J1939Frame f;
    while (otk_link_pop(&b->mc_link, otk_ns_from_ms(b->t_ms), &o)) {
        if (oc_frame_from_otk(&o, &f)) {
            apply_rx(b, &f);
        }
    }
}

void oc_bench_rx(OcBench *b, const J1939Frame *f) {
    if (b->sig.fault_mc_tx_lost) {
        b->rx_dropped++;
        return;
    }
    OtkCanFrame o;
    oc_frame_to_otk(f, &o);
    (void)otk_link_push(&b->mc_link, &o, otk_ns_from_ms(b->t_ms)); /* a lost frame is counted by the link */
    deliver_mc(b);
}

static bool due(uint32_t t_ms, uint32_t period_ms) {
    return period_ms != 0U && (t_ms % period_ms) == 0U;
}

static size_t push(J1939Frame *tx, size_t n, size_t cap, const J1939Frame *f) {
    if (n < cap) {
        tx[n] = *f;
        return n + 1U;
    }
    return n;
}

/* The engine's frame enters the engine link at the current time. */
static void send_engine(OcBench *b, const J1939Frame *f) {
    OtkCanFrame o;
    oc_frame_to_otk(f, &o);
    (void)otk_link_push(&b->engine_link, &o, otk_ns_from_ms(b->t_ms)); /* a lost frame is counted by the link */
}

static size_t transmit(OcBench *b, J1939Frame *tx, size_t cap) {
    const OcBenchConfig *cfg = b->cfg;
    const uint8_t ecu_addr = cfg->ecu->source_address;
    const uint32_t t = b->t_ms;
    size_t n = 0U;
    J1939Frame f;

    /* The engine transmits only while its ECU is powered. */
    if (b->sig.key_on && !b->sig.fault_engine_tx_lost) {
        if (due(t, cfg->eec1_period_ms)) {
            const double rpm = b->plant.rpm;
            const double full_indicated = plant_full_load_brake_nm(cfg->plant, rpm) + cfg->plant->friction_c0_nm +
                                          cfg->plant->friction_c1_nm_per_rpm * rpm;
            const double ref = cfg->plant->reference_torque_nm;
            const J1939Eec1 eec1 = {
                .torque_mode = (b->ecu.source == ECU_SPEED_SOURCE_LOW_IDLE) ? J1939_TORQUE_MODE_LOW_IDLE
                                                                            : J1939_TORQUE_MODE_NA,
                .driver_demand_torque_valid = true,
                .driver_demand_torque_pct = (float)(100.0 * (b->ecu.fuel_pct / 100.0) * full_indicated / ref),
                .actual_torque_valid = true,
                .actual_torque_pct = (float)(100.0 * b->plant.indicated_torque_nm / ref),
                .engine_speed_valid = true,
                .engine_speed_rpm = (float)rpm,
                .controlling_address = b->ecu.controlling_address,
            };
            j1939_encode_eec1(&eec1, ecu_addr, &f);
            send_engine(b, &f);
        }
        if (due(t, cfg->eflp1_period_ms)) {
            const J1939Eflp1 m = { .oil_pressure_valid = true, .oil_pressure_kpa = (float)b->plant.oil_pressure_kpa };
            j1939_encode_eflp1(&m, ecu_addr, &f);
            send_engine(b, &f);
        }
        if (due(t, cfg->et1_period_ms)) {
            const J1939Et1 m = { .coolant_temp_valid = true, .coolant_temp_c = (float)b->plant.coolant_temp_c };
            j1939_encode_et1(&m, ecu_addr, &f);
            send_engine(b, &f);
        }
    }

    /* What the engine link delivers now: with no fault, the frames just sent. */
    OtkCanFrame o;
    while (otk_link_pop(&b->engine_link, otk_ns_from_ms(t), &o)) {
        if (oc_frame_from_otk(&o, &f)) {
            n = push(tx, n, cap, &f);
        }
    }

    /* The operator panel. */
    if (due(t, cfg->operator_period_ms)) {
        const OcOperator op = {
            .target_speed_valid = b->sig.target_rpm > 0.0,
            .target_speed_rpm = (float)b->sig.target_rpm,
            .hoist_demand_valid = true,
            .hoist_demand_pct = (float)b->sig.hoist_demand_pct,
            .fault_reset_pressed = b->sig.fault_reset,
        };
        oc_encode_operator(&op, OC_ADDR_BENCH, &f);
        n = push(tx, n, cap, &f);
    }

    /* The watchdog test frame (test.dut_hang). */
    if (b->sig.test_dut_hang) {
        f.id = OC_TEST_HANG_ID;
        f.dlc = 8U;
        for (size_t i = 0U; i < 8U; i++) {
            f.data[i] = 0xFFU;
        }
        n = push(tx, n, cap, &f);
    }
    return n;
}

size_t oc_bench_step(OcBench *b, J1939Frame *tx, size_t cap) {
    const OcBenchConfig *cfg = b->cfg;
    const double dt = (double)cfg->step_ms / 1000.0;

    /* 0. Scenario events due at the start of this step, then the controller's
     *    frames that the mc link delivers now (delayed ones). */
    apply_due_events(b);
    deliver_mc(b);

    /* 1. The engine ECU decides fuel and starter. */
    b->ecu.ignore_requests = b->sig.fault_ecu_ignores_tsc1;
    const EngineEcuInputs ecu_in = { .key_on = b->sig.key_on, .start = b->sig.start, .rpm = b->plant.rpm };
    EngineEcuOutputs ecu_out;
    engine_ecu_step(&b->ecu, cfg->ecu, &ecu_in, cfg->step_ms, &ecu_out);

    /* 2. The pump answers the machine controller's command and the load on the hook. */
    const bool cmd_fresh = b->pump_cmd_seen && (uint32_t)(b->t_ms - b->pump_cmd_ms) <= cfg->pump_cmd_timeout_ms;
    b->pump_cmd_applied_pct = cmd_fresh ? b->pump_cmd_pct : 0.0;
    const double pump_nm = hydraulic_pump_step(&b->pump, cfg->pump, b->pump_cmd_applied_pct, b->sig.hoist_load_bar,
                                               b->plant.rpm, dt);
    b->load_torque_nm = pump_nm - b->sig.fault_overrun_torque_nm;

    /* 3. Physics. */
    const PlantInputs plant_in = {
        .fuel_pct = ecu_out.fuel_pct,
        .load_torque_nm = b->load_torque_nm,
        .starter_engaged = ecu_out.starter_engaged,
    };
    plant_step(&b->plant, cfg->plant, &plant_in, dt);

    /* 4. Time advances; the machine reports the new state. */
    b->t_ms += cfg->step_ms;
    return transmit(b, tx, cap);
}
