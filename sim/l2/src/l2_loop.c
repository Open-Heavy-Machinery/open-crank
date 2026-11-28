/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file l2_loop.c
 * @brief The Level 2 loop in memory. See l2_loop.h.
 */

#include <opencrank/l2/l2_loop.h>

#include <math.h>
#include <string.h>

#include <opencrank/can_otk.h>
#include <opencrank/oc_msgs.h>
#include <opencrank/plants/d936la6.h>

#define ACC_FULL 281474976710656.0 /* 2^48 */
#define ACC_HALF (UINT64_C(1) << 47)
#define TIMER_HZ_NOMINAL 170000000.0
#define LOOP_QUEUE 16U

const OcL2LoopConfig OC_L2_LOOP_D936LA6 = {
    .plant = &PLANT_D936LA6,
    .l2 = &L2_D936LA6,
    .pump = &HYDRAULIC_PUMP_HOIST,
    .mc = &MC_DEFAULT_PARAMS,
    .ecu = &ECU_DEFAULT_PARAMS,
    .hsi_bias = OC_L2_HSI_BIAS_DEFAULT,
    .capture_delay_steps = 30U,
    .pump_cmd_timeout_ms = 200U,
};

static void apply_signal(void *ctx, OtkSignalId signal, double value) {
    oc_signals_set((OcSignals *)ctx, (OcSignalId)signal, value);
}

/* ---- the edge model --------------------------------------------------------- */

/* The gateware's tooth i boundary, above crank_offset: ceil(i * 2^31 / 60). */
static uint64_t tooth_rel(uint32_t i) {
    return (((uint64_t)i << 31) + (OC_L2_CRANK_TEETH - 1U)) / OC_L2_CRANK_TEETH;
}

/* Unwrapped accumulator of tooth i of revolution r; revolution 0 begins at -60 degrees. */
static int64_t tooth_u(const OcL2Loop *l, int64_t r, uint32_t i) {
    const int64_t top = (int64_t)((uint64_t)l->offset32 + tooth_rel(i));
    return top * 65536 - (int64_t)(UINT64_C(1) << 48) + r * (int64_t)ACC_HALF;
}

static void next_tooth(OcL2Loop *l) {
    l->next_tooth_idx++;
    if (l->next_tooth_idx >= OC_L2_CRANK_TEETH - OC_L2_CRANK_MISSING) {
        l->next_tooth_idx = 0U;
        l->next_tooth_rev++;
    }
    l->next_tooth_u = (uint64_t)tooth_u(l, l->next_tooth_rev, l->next_tooth_idx);
}

/* The bridge's tick in the ECU's timer: 170 MHz, as slow or fast as its HSI. */
static uint32_t ecu_ticks(const OcL2Loop *l, double bridge_tick) {
    const double ratio = TIMER_HZ_NOMINAL * (1.0 + l->cfg->hsi_bias) / (double)OC_L2_TICK_HZ;
    return (uint32_t)fmod(floor(bridge_tick * ratio), 4294967296.0);
}

static double ecu_to_bridge(const OcL2Loop *l, double ecu) {
    const double ratio = TIMER_HZ_NOMINAL * (1.0 + l->cfg->hsi_bias) / (double)OC_L2_TICK_HZ;
    return ecu / ratio;
}

/* The armed injection starts on this crank edge (bridge tick @p b) plus its delay. */
static void inject(OcL2Loop *l, uint64_t b) {
    if (l->n_cap >= OC_L2_CAPTURES) {
        l->captures_lost++;
        return;
    }
    OcL2Capture *c = &l->cap[l->n_cap++];
    const double start = (double)b + ecu_to_bridge(l, (double)l->arm.delay_ticks);
    const double width = ecu_to_bridge(l, (double)l->arm.width_ticks);
    c->start = (uint64_t)llround(start);
    c->width = (uint32_t)llround(width);
    c->channel = (uint8_t)(l->arm.injector - 1U);
    c->due = (uint32_t)((c->start + c->width) / OC_L2_STEP_TICKS) + l->cfg->capture_delay_steps;
    c->width_us_commanded = (float)((double)l->arm.width_ticks / (TIMER_HZ_NOMINAL / 1e6));
}

static void crank_edge(OcL2Loop *l, uint64_t b) {
    if (l->teeth_to_drop > 0U) {
        l->teeth_to_drop--;
        return; /* fault.crank_teeth_lost: the ECU never sees it */
    }
    if (l->arm_pending) {
        l->arm_pending = false;
        inject(l, b);
    }
    l->teeth_seen++;
    EcuArm a;
    if (ecu_wheel_tooth(&l->wheel, ecu_ticks(l, (double)b), &a)) {
        l->arm = a;
        l->arm_pending = true;
    }
}

/* The edges of step k, in time order: the first tick at which the accumulator reaches each boundary. */
static void edges_of_step(OcL2Loop *l, uint32_t k) {
    const OcL2StepRecord *r = &l->plant.hist[k % OC_L2_HISTORY];
    if (r->inc == 0U) {
        return;
    }
    const uint64_t u0 = r->cycle * (UINT64_C(1) << 48) + r->acc;
    const uint64_t u1 = u0 + (uint64_t)r->inc * OC_L2_STEP_TICKS;
    const uint64_t t0 = (uint64_t)k * OC_L2_STEP_TICKS;
    for (;;) {
        const bool cam_first = l->next_cam_u <= l->next_tooth_u;
        const uint64_t u = cam_first ? l->next_cam_u : l->next_tooth_u;
        if (u > u1) {
            break;
        }
        const uint64_t n = (u > u0) ? (u - u0 + r->inc - 1U) / r->inc : 0U;
        const uint64_t b = t0 + n;
        if (cam_first) {
            ecu_wheel_cam(&l->wheel, ecu_ticks(l, (double)b));
            l->next_cam_u += UINT64_C(1) << 48;
        } else {
            crank_edge(l, b);
            next_tooth(l);
        }
    }
}

/* ---- captures to the plant ------------------------------------------------- */

static void deliver_captures(OcL2Loop *l) {
    const OcL2LoopConfig *cfg = l->cfg;
    size_t kept = 0U;
    for (size_t i = 0U; i < l->n_cap; i++) {
        OcL2Capture c = l->cap[i];
        if (c.due > l->plant.step) {
            l->cap[kept++] = c;
            continue;
        }
        double deg = 0.0;
        const OcL2CaptureResult res = oc_l2_capture(&l->plant, cfg->l2, c.channel, c.start, c.width, &deg);
        if (res != OC_L2_CAPTURE_COUNTED) {
            continue;
        }
        double err = deg - (oc_l2_tdc_deg(c.channel + 1U) - (double)cfg->ecu->soi_btdc_deg);
        err = fmod(err + 1080.0, 720.0) - 360.0;
        l->last.valid = true;
        l->last.soi_err_deg = err;
        l->last.width_us = (double)c.width * 1e6 / (double)OC_L2_TICK_HZ;
        const bool commanded = c.width_us_commanded > 0.0f; /* not known with an external ECU */
        l->last.width_err_us = commanded ? l->last.width_us - (double)c.width_us_commanded : 0.0;
        if (!l->first_injection_seen) {
            double u = 0.0;
            l->first_injection_seen = oc_l2_angle_at(&l->plant, c.start, &u);
            l->first_injection_rev = u / 360.0;
            l->first_injection_ms = l->t_ms;
        } else if (l->t_ms >= l->first_injection_ms + 1000U) {
            if (l->soi_err != NULL && l->soi_n < l->soi_cap) {
                l->soi_err[l->soi_n++] = err;
            }
            if (commanded) { /* the width errors need the commanded width */
                const double werr = fabs(l->last.width_err_us);
                if (werr > l->width_err_max_us) {
                    l->width_err_max_us = werr;
                }
                const double rel = werr / (double)c.width_us_commanded;
                if (rel > l->width_rel_err_max) {
                    l->width_rel_err_max = rel;
                }
                const double excess = werr - 0.003 * (double)c.width_us_commanded;
                if (!l->width_measured || excess > l->width_excess_max_us) {
                    l->width_excess_max_us = excess;
                }
                l->width_measured = true;
            }
        } else {
            /* the first second after the first injection: run-up, not counted */
        }
    }
    l->n_cap = kept;
}

/* ---- the machine around the ECU ---------------------------------------------- */

static void push(OtkCanQueue *q, const J1939Frame *f) {
    OtkCanFrame o;
    oc_frame_to_otk(f, &o);
    (void)otk_can_queue_push(q, &o);
}

static size_t controllers(OcL2Loop *l, OtkCanFrame *ecu_tx, size_t cap) {
    const uint32_t now = l->t_ms;
    size_t n_tx = 0U;
    J1939Frame f;

    /* The ECU: the TSC1 sent at the controller's last step, then its governor
     * (in memory only; an external ECU sends its EEC1 through oc_l2_loop_rx()). */
    for (size_t i = 0U; !l->external_ecu && i < otk_can_queue_count(&l->to_ecu); i++) {
        if (oc_frame_from_otk(otk_can_queue_at(&l->to_ecu, i), &f)) {
            ecu_handle_frame(&l->ecu, &f, now);
        }
    }
    otk_can_queue_clear(&l->to_ecu);
    EcuWheelStatus ws;
    ecu_wheel_status(&l->wheel, &ws);
    J1939Frame eec1[1];
    if (!l->external_ecu && ecu_step(&l->ecu, &ws, now, eec1, 1U) == 1U) {
        push(&l->to_mc, &eec1[0]);
        if (n_tx < cap) {
            oc_frame_to_otk(&eec1[0], &ecu_tx[n_tx++]);
        }
        J1939Eec1 m;
        if (j1939_decode_eec1(&eec1[0], &m) && m.engine_speed_valid) {
            l->ecu_eec1_rpm = (double)m.engine_speed_rpm;
            l->ecu_eec1_seen = true;
        }
    }
    if (!l->external_ecu) {
        ecu_wheel_set_width(&l->wheel, ecu_width_ticks(&l->ecu));
    }

    /* The operator panel, every 20 ms, as the Level 1 bench. */
    if ((now % 20U) == 0U) {
        const OcOperator op = {
            .target_speed_valid = l->sig.target_rpm > 0.0,
            .target_speed_rpm = (float)l->sig.target_rpm,
            .hoist_demand_valid = true,
            .hoist_demand_pct = (float)l->sig.hoist_demand_pct,
            .fault_reset_pressed = l->sig.fault_reset,
        };
        oc_encode_operator(&op, OC_ADDR_BENCH, &f);
        push(&l->to_mc, &f);
    }

    /* The machine controller: what reached it, then its step. */
    for (size_t i = 0U; i < otk_can_queue_count(&l->to_mc); i++) {
        if (oc_frame_from_otk(otk_can_queue_at(&l->to_mc, i), &f)) {
            mc_handle_frame(&l->mc, &f, now);
        }
    }
    otk_can_queue_clear(&l->to_mc);
    J1939Frame out[4];
    const size_t n = mc_step(&l->mc, now, out, 4U);
    for (size_t i = 0U; i < n; i++) {
        OcPumpCmd pump;
        if (oc_decode_pump_cmd(&out[i], &pump)) {
            l->pump_cmd_pct = pump.displacement_valid ? (double)pump.displacement_pct : 0.0;
            l->pump_cmd_seen = true;
            l->pump_cmd_ms = now;
        } else if (l->external_ecu) {
            if (n_tx < cap) {
                oc_frame_to_otk(&out[i], &ecu_tx[n_tx++]); /* TSC1 and its DM1, onto the bus */
            }
        } else {
            push(&l->to_ecu, &out[i]); /* TSC1 and the controller's DM1; the ECU takes TSC1 */
        }
    }
    return n_tx;
}

void oc_l2_loop_init(OcL2Loop *l, const OcL2LoopConfig *cfg, const OcScenario *scenario) {
    memset(l, 0, sizeof(*l));
    l->cfg = cfg;
    otk_player_init(&l->player, scenario, apply_signal, &l->sig);
    oc_l2_init(&l->plant, cfg->plant);
    hydraulic_pump_init(&l->pump, cfg->pump);
    mc_init(&l->mc, cfg->mc);
    ecu_wheel_init(&l->wheel, cfg->ecu);
    ecu_init(&l->ecu, cfg->ecu);
    otk_can_queue_init(&l->to_ecu, LOOP_QUEUE);
    otk_can_queue_init(&l->to_mc, LOOP_QUEUE);

    OtkBridgeConfig bc;
    oc_l2_bridge_config(&bc);
    l->offset32 = bc.crank_offset;
    /* the first tooth edge after angle 0: revolution 0, from tooth 0 on */
    l->next_tooth_rev = 0;
    l->next_tooth_idx = 0U;
    l->next_tooth_u = (uint64_t)tooth_u(l, 0, 0U);
    while (tooth_u(l, l->next_tooth_rev, l->next_tooth_idx) <= 0) {
        next_tooth(l);
    }
    l->next_cam_u = (uint64_t)bc.cam_start << 16;
}

void oc_l2_loop_init_external(OcL2Loop *l, const OcL2LoopConfig *cfg, const OcScenario *scenario) {
    oc_l2_loop_init(l, cfg, scenario);
    l->external_ecu = true;
}

bool oc_l2_loop_capture(OcL2Loop *l, unsigned channel, uint64_t start_tick, uint32_t width_ticks) {
    if (l->n_cap >= OC_L2_CAPTURES) {
        l->captures_lost++;
        return false;
    }
    const uint64_t began = start_tick / OC_L2_STEP_TICKS;
    const uint32_t lag = (l->plant.step > began) ? (uint32_t)(l->plant.step - began) : 0U;
    if (lag > l->capture_lag_max) {
        l->capture_lag_max = lag;
    }
    OcL2Capture *c = &l->cap[l->n_cap++];
    c->start = start_tick;
    c->width = width_ticks;
    c->channel = (uint8_t)channel;
    c->due = l->plant.step; /* the next step */
    c->width_us_commanded = 0.0f;
    return true;
}

void oc_l2_loop_rx(OcL2Loop *l, const OtkCanFrame *f) {
    (void)otk_can_queue_push(&l->to_mc, f);
    J1939Frame j;
    J1939Eec1 m;
    if (oc_frame_from_otk(f, &j) && j1939_id_source(j.id) == l->cfg->ecu->source_address &&
        j1939_decode_eec1(&j, &m) && m.engine_speed_valid) {
        l->ecu_eec1_rpm = (double)m.engine_speed_rpm;
        l->ecu_eec1_seen = true;
    }
}

void oc_l2_loop_keep_errors(OcL2Loop *l, double *buf, size_t cap) {
    l->soi_err = buf;
    l->soi_cap = cap;
    l->soi_n = 0U;
}

size_t oc_l2_loop_step(OcL2Loop *l, OtkCanFrame *ecu_tx, size_t cap) {
    const OcL2LoopConfig *cfg = l->cfg;
    const double dt = OC_L2_STEP_S;

    deliver_captures(l);
    otk_player_advance(&l->player, otk_ns_from_ms(l->t_ms));
    if (l->sig.fault_crank_teeth_lost != l->last_drop_signal) {
        l->last_drop_signal = l->sig.fault_crank_teeth_lost;
        if (l->sig.fault_crank_teeth_lost >= 1.0) {
            l->teeth_to_drop += (uint32_t)l->sig.fault_crank_teeth_lost;
        }
    }

    /* The pump answers the controller's last command (stale after 200 ms: 0). */
    const bool fresh = l->pump_cmd_seen && (uint32_t)(l->t_ms - l->pump_cmd_ms) <= cfg->pump_cmd_timeout_ms;
    const double pump_nm =
        hydraulic_pump_step(&l->pump, cfg->pump, fresh ? l->pump_cmd_pct : 0.0, l->sig.hoist_load_bar,
                            l->plant.plant.rpm, dt);
    l->load_torque_nm = pump_nm - l->sig.fault_overrun_torque_nm;

    const uint32_t k = l->plant.step;
    const OcL2Inputs in = { .load_torque_nm = l->load_torque_nm, .starter_engaged = l->sig.start };
    (void)oc_l2_step(&l->plant, cfg->plant, cfg->l2, &in);

    /* The ECU in the capture interrupt: the step's edges, then its poll (in memory only). */
    if (!l->external_ecu) {
        ecu_wheel_poll(&l->wheel, ecu_ticks(l, (double)k * OC_L2_STEP_TICKS));
        edges_of_step(l, k);
    }

    size_t n_tx = 0U;
    if ((l->t_ms % 10U) == 0U) {
        n_tx = controllers(l, ecu_tx, cap);
    }

    /* Misfires in the last second. */
    const size_t slot = (size_t)(k % 1000U);
    l->misfires_1s = l->plant.misfires - l->misfire_hist[slot];
    l->misfire_hist[slot] = l->plant.misfires;

    l->t_ms++;
    return n_tx;
}

bool oc_l2_loop_done(const OcL2Loop *l) {
    return otk_player_done(&l->player, otk_ns_from_ms(l->t_ms));
}

double oc_l2_loop_time_s(const OcL2Loop *l) {
    return (double)l->t_ms / 1000.0;
}
