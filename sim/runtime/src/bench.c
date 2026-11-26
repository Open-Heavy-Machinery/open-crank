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
    .dm1_period_ms = 1000U,
};

/* The engine's diagnostic trouble codes (fault.engine_dtc_*). */
#define DTC_SPN_OIL_PRESSURE 100U  /* SPN 100, engine oil pressure */
#define DTC_FMI_LOW_SEVERE 1U      /* FMI 1, below normal, most severe: red stop lamp */
#define DTC_SPN_COOLANT_TEMP 110U  /* SPN 110, engine coolant temperature */
#define DTC_FMI_HIGH_MODERATE 16U  /* FMI 16, above normal, moderately severe: amber lamp */

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

void oc_bench_set_frame_log(OcBench *b, OtkFrameLog *log) {
    b->frames = log;
    b->mc_in_flight_n = 0U;
}

static void log_row(const OcBench *b, OtkNs t, OtkNs t_bus, OtkFrameDir dir, const char *link,
                    const OtkCanFrame *f, OtkFrameFault fault) {
    if (b->frames != NULL) {
        const OtkFrameRow row = { t, t_bus, dir, link, f, fault };
        (void)otk_framelog_frame(b->frames, &row); /* an error is sticky: the runner reports it */
    }
}

/* The bus time of a frame leaving the mc link: same-id frames never overtake
 * each other, so the first entry with its push time and identifier is it. */
static OtkNs take_in_flight(OcBench *b, OtkNs pushed, uint32_t id) {
    for (size_t i = 0U; i < b->mc_in_flight_n; i++) {
        if (b->mc_in_flight[i].pushed == pushed && b->mc_in_flight[i].id == id) {
            const OtkNs t_bus = b->mc_in_flight[i].t_bus;
            memmove(&b->mc_in_flight[i], &b->mc_in_flight[i + 1U],
                    (b->mc_in_flight_n - i - 1U) * sizeof(b->mc_in_flight[0]));
            b->mc_in_flight_n--;
            return t_bus;
        }
    }
    return OTK_FRAMELOG_NO_TIME;
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
    const OtkNs now = otk_ns_from_ms(b->t_ms);
    OtkCanFrame o;
    OtkLinkInfo info;
    J1939Frame f;
    while (otk_link_pop_ex(&b->mc_link, now, &o, &info)) {
        if (b->frames != NULL) {
            const OtkNs t_bus = take_in_flight(b, info.pushed, o.id);
            log_row(b, now, t_bus, OTK_FRAME_FROM_DUT, "mc", &o, otk_frame_fault_from_link(&info));
        }
        if (info.lost != OTK_LINK_LOST_NONE) {
            continue; /* due while the bus is off: lost, counted by the link */
        }
        if (oc_frame_from_otk(&o, &f)) {
            apply_rx(b, &f);
        }
    }
}

void oc_bench_rx(OcBench *b, const J1939Frame *f) {
    oc_bench_rx_at(b, f, OTK_FRAMELOG_NO_TIME);
}

void oc_bench_rx_at(OcBench *b, const J1939Frame *f, OtkNs t_bus) {
    const OtkNs now = otk_ns_from_ms(b->t_ms);
    OtkCanFrame o;
    oc_frame_to_otk(f, &o);
    if (b->sig.fault_mc_tx_lost) {
        b->rx_dropped++;
        /* fault.mc_tx_lost is the bench's own drop, before any link */
        log_row(b, now, t_bus, OTK_FRAME_FROM_DUT, NULL, &o, OTK_FRAME_DROP);
        return;
    }
    OtkLinkInfo info;
    if (otk_link_push_ex(&b->mc_link, &o, now, &info)) {
        if (b->frames != NULL && b->mc_in_flight_n < OTK_CAN_QUEUE_MAX) {
            const OcBenchInFlight in = { now, o.id, t_bus };
            b->mc_in_flight[b->mc_in_flight_n++] = in;
        }
    } else {
        log_row(b, now, t_bus, OTK_FRAME_FROM_DUT, "mc", &o, otk_frame_fault_from_link(&info)); /* counted by the link */
    }
    deliver_mc(b);
}

static bool due(uint32_t t_ms, uint32_t period_ms) {
    return period_ms != 0U && (t_ms % period_ms) == 0U;
}

static size_t push(J1939Frame *tx, OcBenchTxInfo *info, size_t n, size_t cap, const J1939Frame *f,
                   const OcBenchTxInfo *what) {
    if (n < cap) {
        tx[n] = *f;
        if (info != NULL) {
            info[n] = *what;
        }
        return n + 1U;
    }
    return n;
}

/* The engine's frame enters the engine link at the current time. */
static void send_engine(OcBench *b, const J1939Frame *f) {
    OtkCanFrame o;
    oc_frame_to_otk(f, &o);
    OtkLinkInfo info;
    if (!otk_link_push_ex(&b->engine_link, &o, otk_ns_from_ms(b->t_ms), &info)) {
        /* lost, counted by the link: it never reaches the bus */
        log_row(b, otk_ns_from_ms(b->t_ms), OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "engine", &o,
                otk_frame_fault_from_link(&info));
    }
}

/* DM1 every dm1_period_ms and at once on a change; with two codes it is a
 * BAM whose packets go J1939_TP_BAM_GAP_MS apart. One BAM at a time. */
static void send_dm1(OcBench *b, uint32_t t) {
    const bool red = b->sig.fault_engine_dtc_red;
    const bool amber = b->sig.fault_engine_dtc_amber;
    const bool changed = !b->dm1_sent || red != b->dm1_red || amber != b->dm1_amber;
    if (changed || (b->cfg->dm1_period_ms != 0U && (t % b->cfg->dm1_period_ms) == 0U)) {
        b->dm1_pending = true;
    }
    if (b->bam_next < b->bam_n) {
        if (t >= b->bam_next_ms) {
            send_engine(b, &b->bam[b->bam_next]);
            b->bam_next++;
            b->bam_next_ms = t + J1939_TP_BAM_GAP_MS;
        }
        return; /* the next DM1 waits for this BAM */
    }
    if (!b->dm1_pending) {
        return;
    }
    J1939Dm1 dm1 = { 0 };
    dm1.mil = J1939_LAMP_OFF;
    dm1.red_stop = red ? J1939_LAMP_ON : J1939_LAMP_OFF;
    dm1.amber_warning = amber ? J1939_LAMP_ON : J1939_LAMP_OFF;
    dm1.protect = J1939_LAMP_OFF;
    if (red) {
        dm1.dtc[dm1.n_dtc++] = (J1939Dtc){ DTC_SPN_OIL_PRESSURE, DTC_FMI_LOW_SEVERE, 1U };
    }
    if (amber) {
        dm1.dtc[dm1.n_dtc++] = (J1939Dtc){ DTC_SPN_COOLANT_TEMP, DTC_FMI_HIGH_MODERATE, 1U };
    }
    uint8_t data[J1939_DM1_MAX_LEN];
    const size_t len = j1939_encode_dm1(&dm1, data, sizeof(data));
    if (len == 8U) {
        J1939Frame f;
        f.id = j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_DM1, J1939_ADDR_GLOBAL, b->cfg->ecu->source_address);
        f.dlc = 8U;
        for (size_t i = 0U; i < 8U; i++) {
            f.data[i] = data[i];
        }
        send_engine(b, &f);
    } else {
        b->bam_n = (uint8_t)j1939_bam_encode(J1939_PGN_DM1, data, len, b->cfg->ecu->source_address, b->bam,
                                             sizeof(b->bam) / sizeof(b->bam[0]));
        b->bam_next = 0U;
        if (b->bam_n > 0U) {
            send_engine(b, &b->bam[0]); /* TP.CM now, TP.DT later */
            b->bam_next = 1U;
            b->bam_next_ms = t + J1939_TP_BAM_GAP_MS;
        }
    }
    b->dm1_sent = true;
    b->dm1_red = red;
    b->dm1_amber = amber;
    b->dm1_pending = false;
}

static size_t transmit(OcBench *b, J1939Frame *tx, OcBenchTxInfo *txi, size_t cap) {
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
        send_dm1(b, t);
    }

    /* What the engine link delivers now: with no fault, the frames just sent. */
    OtkCanFrame o;
    OtkLinkInfo info;
    while (otk_link_pop_ex(&b->engine_link, otk_ns_from_ms(t), &o, &info)) {
        if (info.lost != OTK_LINK_LOST_NONE) {
            log_row(b, info.pushed, OTK_FRAMELOG_NO_TIME, OTK_FRAME_TO_DUT, "engine", &o,
                    otk_frame_fault_from_link(&info));
            continue; /* due while the bus is off: lost, counted by the link */
        }
        if (oc_frame_from_otk(&o, &f)) {
            const OcBenchTxInfo what = { info.pushed, "engine", otk_frame_fault_from_link(&info) };
            n = push(tx, txi, n, cap, &f, &what);
        }
    }
    const OcBenchTxInfo direct = { otk_ns_from_ms(t), NULL, OTK_FRAME_OK }; /* no link */

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
        n = push(tx, txi, n, cap, &f, &direct);
    }

    /* The watchdog test frame (test.dut_hang). */
    if (b->sig.test_dut_hang) {
        f.id = OC_TEST_HANG_ID;
        f.dlc = 8U;
        for (size_t i = 0U; i < 8U; i++) {
            f.data[i] = 0xFFU;
        }
        n = push(tx, txi, n, cap, &f, &direct);
    }
    return n;
}

size_t oc_bench_step(OcBench *b, J1939Frame *tx, size_t cap) {
    return oc_bench_step_ex(b, tx, NULL, cap);
}

size_t oc_bench_step_ex(OcBench *b, J1939Frame *tx, OcBenchTxInfo *info, size_t cap) {
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
    return transmit(b, tx, info, cap);
}
