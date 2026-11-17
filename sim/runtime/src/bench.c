/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file bench.c
 * @brief The simulated machine as seen from the CAN bus. See bench.h.
 */

#include <opencrank/runtime/bench.h>

#include <string.h>

#include <opencrank/oc_msgs.h>
#include <opencrank/plants/d936la6.h>

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
}

double oc_bench_time_s(const OcBench *b) {
    return (double)b->t_ms / 1000.0;
}

void oc_bench_rx(OcBench *b, const J1939Frame *f) {
    if (b->sig.fault_mc_tx_lost) {
        b->rx_dropped++;
        return;
    }

    J1939Tsc1 tsc1;
    OcPumpCmd pump;
    const uint8_t dest = j1939_id_dest(f->id);
    const uint8_t ecu_addr = b->cfg->ecu->source_address;

    if ((dest == ecu_addr || dest == J1939_ADDR_GLOBAL) && j1939_decode_tsc1(f, &tsc1)) {
        if (!b->sig.key_on) {
            return; /* an unpowered ECU hears nothing */
        }
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

static size_t transmit(const OcBench *b, J1939Frame *tx, size_t cap) {
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
            n = push(tx, n, cap, &f);
        }
        if (due(t, cfg->eflp1_period_ms)) {
            const J1939Eflp1 m = { .oil_pressure_valid = true, .oil_pressure_kpa = (float)b->plant.oil_pressure_kpa };
            j1939_encode_eflp1(&m, ecu_addr, &f);
            n = push(tx, n, cap, &f);
        }
        if (due(t, cfg->et1_period_ms)) {
            const J1939Et1 m = { .coolant_temp_valid = true, .coolant_temp_c = (float)b->plant.coolant_temp_c };
            j1939_encode_et1(&m, ecu_addr, &f);
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
    return n;
}

size_t oc_bench_step(OcBench *b, J1939Frame *tx, size_t cap) {
    const OcBenchConfig *cfg = b->cfg;
    const double dt = (double)cfg->step_ms / 1000.0;

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
